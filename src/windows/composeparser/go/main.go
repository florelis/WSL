// Copyright (C) Microsoft Corporation. All rights reserved.
package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"regexp"
	"strings"
	"sync"
	"unicode/utf8"

	"github.com/sirupsen/logrus"
)

const (
	maxRequest  = 4 * 1024 * 1024
	maxResponse = 16 * 1024 * 1024
	maxDepth    = 64
	maxNodes    = 100000
)

type source struct {
	ID       string `json:"id"`
	Filename string `json:"filename"`
	Content  string `json:"content"`
}
type request struct {
	Version     int               `json:"version"`
	Sources     []source          `json:"sources"`
	Name        string            `json:"project_name"`
	Directory   string            `json:"project_directory"`
	Environment map[string]string `json:"environment"`
	Profiles    []string          `json:"profiles"`
}
type diagnostic struct {
	Code     string `json:"code"`
	Severity string `json:"severity"`
	Message  string `json:"message"`
	SourceID string `json:"source_id"`
	Path     string `json:"path"`
}
type result struct {
	Version     int            `json:"version"`
	Status      string         `json:"status"`
	Diagnostics []diagnostic   `json:"diagnostics"`
	Project     map[string]any `json:"project,omitempty"`
}
type parseError struct{ code, message, path string }

func (e *parseError) Error() string      { return e.message }
func invalid(path, message string) error { return &parseError{"invalid_compose", message, path} }
func unsupported(path string) error {
	return &parseError{"unsupported_feature", "Outside the supported POC slice: " + path, path}
}
func failure(code, message, id, path string) result {
	return result{1, "error", []diagnostic{{code, "error", message, id, path}}, nil}
}
func errorResult(err error, id string) result {
	var e *parseError
	if errors.As(err, &e) {
		return failure(e.code, e.message, id, e.path)
	}
	return failure("invalid_compose", err.Error(), id, "")
}

// The DLL serializes interpretation because compose-go uses the default logger.
type warningSink struct{ diagnostics []diagnostic }

var parserLock sync.Mutex
var warnings warningSink

func init() {
	logrus.SetOutput(io.Discard)
	logrus.AddHook(&warnings)
}

func (w *warningSink) Levels() []logrus.Level { return []logrus.Level{logrus.WarnLevel} }
func (w *warningSink) Fire(e *logrus.Entry) error {
	w.diagnostics = append(w.diagnostics, diagnostic{"invalid_compose", "warning", e.Message, "", ""})
	return nil
}

func readJSON(d *json.Decoder, depth int) (any, error) {
	if depth > 128 {
		return nil, fmt.Errorf("JSON nesting exceeds 128")
	}
	token, err := d.Token()
	if err != nil {
		return nil, err
	}
	delim, ok := token.(json.Delim)
	if !ok {
		return token, nil
	}
	switch delim {
	case '{':
		m := map[string]any{}
		for d.More() {
			k, err := d.Token()
			if err != nil {
				return nil, err
			}
			key, ok := k.(string)
			if !ok {
				return nil, fmt.Errorf("Nonstring JSON key")
			}
			if _, exists := m[key]; exists {
				return nil, fmt.Errorf("Duplicate JSON key: %s", key)
			}
			value, err := readJSON(d, depth+1)
			if err != nil {
				return nil, err
			}
			m[key] = value
		}
		_, err = d.Token()
		return m, err
	case '[':
		a := []any{}
		for d.More() {
			v, err := readJSON(d, depth+1)
			if err != nil {
				return nil, err
			}
			a = append(a, v)
		}
		_, err = d.Token()
		return a, err
	default:
		return nil, fmt.Errorf("Unexpected JSON delimiter")
	}
}

func decodeRequest(data []byte) (request, error) {
	var r request
	if !utf8.Valid(data) {
		return r, fmt.Errorf("Request is not UTF-8")
	}
	d := json.NewDecoder(bytes.NewReader(data))
	d.UseNumber()
	v, err := readJSON(d, 0)
	if err != nil {
		return r, err
	}
	if _, err = d.Token(); err != io.EOF {
		return r, fmt.Errorf("Trailing JSON data")
	}
	m, ok := v.(map[string]any)
	if !ok || len(m) != 6 {
		return r, fmt.Errorf("Invalid request shape")
	}
	for _, k := range []string{"version", "sources", "project_name", "project_directory", "environment", "profiles"} {
		if m[k] == nil {
			return r, fmt.Errorf("Missing/null field: %s", k)
		}
	}
	if m["version"] != json.Number("1") {
		return r, fmt.Errorf("Unsupported version")
	}
	for _, k := range []string{"project_name", "project_directory"} {
		if _, ok := m[k].(string); !ok {
			return r, fmt.Errorf("%s must be a string", k)
		}
	}
	environment, ok := m["environment"].(map[string]any)
	if !ok {
		return r, fmt.Errorf("Environment must be an object")
	}
	for _, v := range environment {
		if _, ok := v.(string); !ok {
			return r, fmt.Errorf("Environment values must be strings")
		}
	}
	profiles, ok := m["profiles"].([]any)
	if !ok {
		return r, fmt.Errorf("Profiles must be an array")
	}
	for _, v := range profiles {
		if _, ok := v.(string); !ok {
			return r, fmt.Errorf("Profiles must contain strings")
		}
	}
	d = json.NewDecoder(bytes.NewReader(data))
	d.DisallowUnknownFields()
	if err := d.Decode(&r); err != nil {
		return r, err
	}
	if len(r.Sources) == 0 || !regexp.MustCompile(`^[a-z0-9][a-z0-9_-]*$`).MatchString(r.Name) || !absoluteWindowsPath(r.Directory) {
		return r, fmt.Errorf("Invalid sources, project name, or explicit Windows directory")
	}
	ids := map[string]bool{}
	for _, s := range r.Sources {
		if s.ID == "" || s.Content == "" || ids[s.ID] {
			return r, fmt.Errorf("Sources need unique nonempty IDs and contents")
		}
		ids[s.ID] = true
	}
	sources, ok := m["sources"].([]any)
	if !ok {
		return r, fmt.Errorf("Sources must be an array")
	}
	for _, s := range sources {
		sm, ok := s.(map[string]any)
		if !ok || len(sm) != 3 {
			return r, fmt.Errorf("Invalid source shape")
		}
		for _, k := range []string{"id", "filename", "content"} {
			if _, ok := sm[k].(string); !ok {
				return r, fmt.Errorf("Source %s must be a string", k)
			}
		}
	}
	for _, p := range r.Profiles {
		if p != "*" && !profilePattern.MatchString(p) {
			return r, fmt.Errorf("Invalid selected profile")
		}
	}
	return r, nil
}

func recoverResponse(response *result) {
	if recover() != nil {
		*response = failure("internal_error", "Parser panic", "", "")
	}
}

func interpret(data []byte) (response result) {
	parserLock.Lock()
	defer parserLock.Unlock()
	warnings.diagnostics = nil
	defer func() { warnings.diagnostics = nil }()
	defer recoverResponse(&response)

	if len(data) > maxRequest {
		response = failure("limit_exceeded", "Request exceeds 4 MiB", "", "")
	} else if r, err := decodeRequest(data); err != nil {
		response = failure("invalid_request", err.Error(), "", "")
	} else {
		response = normalize(context.Background(), r)
		response.Diagnostics = append(response.Diagnostics, warnings.diagnostics...)
	}
	return response
}

func encodeResponse(response result) ([]byte, bool) {
	encoded, err := json.Marshal(response)
	if err != nil {
		return nil, false
	}
	if len(encoded) > maxResponse {
		response = failure("limit_exceeded", "Response exceeds 16 MiB", "", "")
		encoded, err = json.Marshal(response)
		if err != nil {
			return nil, false
		}
	}
	return encoded, response.Status == "success"
}

func main() {}

var profilePattern = regexp.MustCompile(`^[a-zA-Z0-9][a-zA-Z0-9_.-]*$`)

func absoluteWindowsPath(s string) bool {
	if strings.ContainsRune(s, 0) {
		return false
	}
	if len(s) >= 3 && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')) && s[1] == ':' && (s[2] == '\\' || s[2] == '/') {
		return true
	}
	if strings.HasPrefix(s, `\\`) && !strings.HasPrefix(s, `\\?`) && !strings.HasPrefix(s, `\\.`) {
		p := strings.Split(s[2:], `\`)
		return len(p) >= 2 && p[0] != "" && p[1] != ""
	}
	return false
}
