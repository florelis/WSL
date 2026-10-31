// Copyright (C) Microsoft Corporation. All rights reserved.
package main

import (
	"encoding/json"
	"fmt"
	"strings"
	"sync"
	"testing"

	"github.com/sirupsen/logrus"
)

func TestRepeatedConcurrentWarnings(t *testing.T) {
	var calls sync.WaitGroup
	for i := 0; i < 8; i++ {
		calls.Add(1)
		go func(i int) {
			defer calls.Done()
			for j := 0; j < 20; j++ {
				r := baseRequest()
				name := fmt.Sprintf("MISSING_%d", i)
				r.Environment["IMAGE"] = fmt.Sprintf("image%d", i)
				r.Sources[0].Content = "services: {app: {image: '$IMAGE', labels: {value: '${OUTER:-${" + name + "}}'}}}"
				data, _ := json.Marshal(r)
				got := interpret(data)
				if got.Status != "success" || len(got.Diagnostics) != 1 || !strings.Contains(got.Diagnostics[0].Message, `"`+name+`"`) {
					t.Errorf("Request %d warning contamination: %+v", i, got)
					return
				}
				image := got.Project["services"].(map[string]any)["app"].(map[string]any)["image"]
				if image != r.Environment["IMAGE"] {
					t.Errorf("Request %d environment contamination: %v", i, image)
				}
				if bad := interpret([]byte("{")); bad.Status != "error" || len(bad.Diagnostics) != 1 {
					t.Errorf("Malformed request inherited warnings: %+v", bad)
				}
				r.Sources[0].Content = "services: {app: {image: alpine}}"
				data, _ = json.Marshal(r)
				if clean := interpret(data); clean.Status != "success" || len(clean.Diagnostics) != 0 {
					t.Errorf("Clean request inherited warnings: %+v", clean)
				}
			}
		}(i)
	}
	calls.Wait()
}

type panicHook struct{}

func (panicHook) Levels() []logrus.Level   { return []logrus.Level{logrus.WarnLevel} }
func (panicHook) Fire(*logrus.Entry) error { panic("test-only parser failure") }

func TestPanicRecoveryAndCleanup(t *testing.T) {
	logger := logrus.StandardLogger()
	saved := logger.Hooks
	hooks := logrus.LevelHooks{}
	hooks.Add(&warnings)
	hooks.Add(panicHook{})
	logger.ReplaceHooks(hooks)
	defer logger.ReplaceHooks(saved)

	r := baseRequest()
	r.Sources[0].Content = "services: {app: {image: alpine, labels: {value: '$UNSET'}}}"
	data, _ := json.Marshal(r)
	got := interpret(data)
	if got.Status != "error" || got.Project != nil || len(got.Diagnostics) != 1 || got.Diagnostics[0].Code != "internal_error" {
		t.Fatalf("Panic escaped request boundary: %+v", got)
	}
	logger.ReplaceHooks(saved)
	data, _ = json.Marshal(baseRequest())
	got = interpret(data)
	if got.Status != "success" || len(got.Diagnostics) != 0 {
		t.Fatalf("Request state survived panic: %+v", got)
	}
}
