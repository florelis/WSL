// Copyright (C) Microsoft Corporation. All rights reserved.
package main

import (
	"bytes"
	"context"
	"encoding/json"
	"os"
	"strings"
	"testing"
)

func baseRequest() request {
	return request{Version: 1, Sources: []source{{"base", `Z:\never-open.yaml`, "services: {app: {image: alpine}}"}},
		Name: "demo", Directory: `C:\captured\project`, Environment: map[string]string{}, Profiles: []string{}}
}

func TestEnvelope(t *testing.T) {
	valid, err := json.Marshal(baseRequest())
	if err != nil {
		t.Fatal(err)
	}
	for _, text := range []string{
		string(valid) + "{}",
		strings.Replace(string(valid), `"version":1`, `"version":1,"version":1`, 1),
		strings.Replace(string(valid), `"version":1`, `"version":2`, 1),
		strings.Replace(string(valid), `"environment":{}`, `"environment":{"X":null}`, 1),
		strings.Replace(string(valid), `"profiles":[]`, `"profiles":[null]`, 1),
		strings.Replace(string(valid), `"filename":`, `"unknown":`, 1),
	} {
		if _, err := decodeRequest([]byte(text)); err == nil {
			t.Errorf("Accepted invalid request: %s", text)
		}
	}
	output, success := encodeResponse(interpret(valid))
	if !success {
		t.Fatalf("Failed: %s", output)
	}
	var response result
	if err := json.Unmarshal(output, &response); err != nil || response.Status != "success" {
		t.Fatalf("%v: %s", err, output)
	}
}

func TestGolden(t *testing.T) {
	data, err := os.ReadFile(`testdata/basic.json`)
	if err != nil {
		t.Fatal(err)
	}
	var fixture struct {
		Request request        `json:"request"`
		Project map[string]any `json:"project"`
	}
	if err := json.Unmarshal(data, &fixture); err != nil {
		t.Fatal(err)
	}
	got := normalize(context.Background(), fixture.Request)
	actual, err := json.Marshal(got.Project)
	if err != nil {
		t.Fatal(err)
	}
	expected, err := json.Marshal(fixture.Project)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(actual, expected) {
		t.Fatalf("Golden mismatch:\n%s\n%+v", actual, got.Diagnostics)
	}
}

func TestNoAmbientEnvironment(t *testing.T) {
	t.Setenv("POC_AMBIENT_ONLY", "must-not-appear")
	r := baseRequest()
	r.Sources[0].Content = "services: {app: {image: '${POC_AMBIENT_ONLY:-captured}'}}"
	got := normalize(context.Background(), r)
	if got.Status != "success" || got.Project["services"].(map[string]any)["app"].(map[string]any)["image"] != "captured" {
		t.Fatalf("%+v", got)
	}
}
