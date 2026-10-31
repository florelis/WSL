// Copyright (C) Microsoft Corporation. All rights reserved.
package main

import (
	"fmt"
	"io"
	"net"
	"regexp"
	"strconv"
	"strings"

	"github.com/compose-spec/compose-go/v2/format"
	"github.com/compose-spec/compose-go/v2/types"
	"go.yaml.in/yaml/v4"
)

type yamlReader struct {
	visits int
	active map[*yaml.Node]bool
}

func (r *yamlReader) expand(n *yaml.Node, depth int) (any, error) {
	if depth > maxDepth || r.visits >= maxNodes {
		return nil, &parseError{"limit_exceeded", "YAML structural limit exceeded", ""}
	}
	r.visits++
	if r.active[n] {
		return nil, &parseError{"invalid_yaml", "Cyclic YAML alias", ""}
	}
	r.active[n] = true
	defer delete(r.active, n)
	if n.Kind == yaml.AliasNode {
		// Count the expanded target, not an extra reference wrapper.
		r.visits--
		return r.expand(n.Alias, depth)
	}
	switch n.Kind {
	case yaml.MappingNode:
		if n.Tag != "!!map" {
			return nil, unsupported("YAML tag " + n.Tag)
		}
		m := map[string]any{}
		explicit := map[string]bool{}
		var merges []map[string]any
		for i := 0; i < len(n.Content); i += 2 {
			k, v := n.Content[i], n.Content[i+1]
			if k.Kind != yaml.ScalarNode || (k.Tag != "!!str" && k.Tag != "!!merge") {
				return nil, &parseError{"invalid_yaml", "Mapping keys must be strings", ""}
			}
			if explicit[k.Value] {
				return nil, &parseError{"invalid_yaml", "Duplicate YAML key: " + k.Value, ""}
			}
			explicit[k.Value] = true
			// Keys count toward the budget, including the YAML merge key.
			r.visits++
			if r.visits > maxNodes || depth+1 > maxDepth {
				return nil, &parseError{"limit_exceeded", "YAML structural limit exceeded", ""}
			}
			value, err := r.expand(v, depth+1)
			if err != nil {
				return nil, err
			}
			if k.Tag == "!!merge" {
				switch merged := value.(type) {
				case map[string]any:
					merges = append(merges, merged)
				case []any:
					for _, entry := range merged {
						object, ok := entry.(map[string]any)
						if !ok {
							return nil, &parseError{"invalid_yaml", "Merge sequence requires mappings", ""}
						}
						merges = append(merges, object)
					}
				default:
					return nil, &parseError{"invalid_yaml", "Merge requires mapping or mapping sequence", ""}
				}
			} else {
				m[k.Value] = value
			}
		}
		for _, merge := range merges {
			for k, v := range merge {
				if _, exists := m[k]; !exists {
					m[k] = v
				}
			}
		}
		return m, nil
	case yaml.SequenceNode:
		if n.Tag != "!!seq" {
			return nil, unsupported("YAML tag " + n.Tag)
		}
		a := []any{}
		for _, child := range n.Content {
			v, err := r.expand(child, depth+1)
			if err != nil {
				return nil, err
			}
			a = append(a, v)
		}
		return a, nil
	case yaml.ScalarNode:
		switch n.Tag {
		case "!!str":
			return n.Value, nil
		case "!!null":
			return nil, nil
		case "!!bool", "!!int", "!!float":
			var value any
			if err := n.Decode(&value); err != nil {
				return nil, &parseError{"invalid_yaml", err.Error(), ""}
			}
			return value, nil
		default:
			return nil, unsupported("YAML tag " + n.Tag)
		}
	default:
		return nil, &parseError{"invalid_yaml", "Unexpected YAML node", ""}
	}
}

func (r *yamlReader) read(content string) (map[string]any, error) {
	decoder := yaml.NewDecoder(strings.NewReader(content))
	var doc yaml.Node
	if err := decoder.Decode(&doc); err != nil {
		return nil, &parseError{"invalid_yaml", err.Error(), ""}
	}
	var extra yaml.Node
	if err := decoder.Decode(&extra); err != io.EOF {
		return nil, &parseError{"invalid_yaml", "Exactly one YAML document is required", ""}
	}
	if len(doc.Content) != 1 {
		return nil, &parseError{"invalid_yaml", "Empty YAML document", ""}
	}
	value, err := r.expand(doc.Content[0], 1)
	if err != nil {
		return nil, err
	}
	m, ok := value.(map[string]any)
	if !ok {
		return nil, invalid("", "Compose root must be a mapping")
	}
	return m, nil
}

func fields(value any, path string, allowed ...string) (map[string]any, error) {
	m, ok := value.(map[string]any)
	if !ok {
		return nil, invalid(path, "Expected mapping at "+path)
	}
	for k := range m {
		found := false
		for _, a := range allowed {
			found = found || k == a
		}
		if !found {
			return nil, unsupported(path + "." + k)
		}
	}
	return m, nil
}
func stringsOnly(value any, path string) error {
	a, ok := value.([]any)
	if !ok {
		return invalid(path, "Expected string array")
	}
	for _, v := range a {
		if _, ok := v.(string); !ok {
			return invalid(path, "Expected string array")
		}
	}
	return nil
}
func stringValue(v any, path string) (string, error) {
	s, ok := v.(string)
	if !ok {
		return "", invalid(path, "Expected string")
	}
	return s, nil
}
func number(v any, path string) (int, error) {
	s := fmt.Sprint(v)
	if !regexp.MustCompile(`^[0-9]+$`).MatchString(s) {
		return 0, unsupported(path)
	}
	n, err := strconv.Atoi(s)
	if err != nil || n < 1 || n > 65535 {
		return 0, invalid(path, "Port outside 1..65535")
	}
	return n, nil
}
func ipv4(s string) bool { return !strings.Contains(s, ":") && net.ParseIP(s).To4() != nil }
func checkPath(s, path string) error {
	if s == "" || strings.ContainsRune(s, 0) {
		return invalid(path, "Empty/invalid bind source")
	}
	if absoluteWindowsPath(s) {
		return nil
	}
	if strings.Contains(s, ":") || strings.HasPrefix(s, "~") || strings.HasPrefix(s, "/") || strings.HasPrefix(s, `\`) {
		return unsupported(path)
	}
	return nil
}

func admitPort(value any, path string) error {
	if m, ok := value.(map[string]any); ok {
		if _, err := fields(m, path, "target", "published", "host_ip", "protocol"); err != nil {
			return err
		}
		if _, err := number(m["target"], path+".target"); err != nil {
			return err
		}
		if p, exists := m["published"]; exists {
			if _, err := number(p, path+".published"); err != nil {
				return err
			}
		}
		if p, exists := m["protocol"]; exists && p != "tcp" && p != "udp" {
			return unsupported(path + ".protocol")
		}
		if h, exists := m["host_ip"]; exists {
			s, ok := h.(string)
			if !ok || !ipv4(s) {
				return unsupported(path + ".host_ip")
			}
		}
		return nil
	}
	s := fmt.Sprint(value)
	if strings.ContainsAny(s, "-[]") || strings.Count(s, ":") > 2 {
		return unsupported(path)
	}
	parts := strings.Split(s, "/")
	if len(parts) > 2 || (len(parts) == 2 && parts[1] != "tcp" && parts[1] != "udp") {
		return unsupported(path)
	}
	endpoints := strings.Split(parts[0], ":")
	if len(endpoints) == 3 && !ipv4(endpoints[0]) {
		return unsupported(path)
	}
	for _, port := range endpoints[max(0, len(endpoints)-2):] {
		if _, err := number(port, path); err != nil {
			return err
		}
	}
	ports, err := types.ParsePortConfig(s)
	if err != nil {
		return invalid(path, err.Error())
	}
	if len(ports) != 1 {
		return unsupported(path)
	}
	return nil
}

func admitMount(value any, path string) error {
	var kind, src, target string
	if s, ok := value.(string); ok {
		parts := strings.Split(s, ":")
		if len(parts) > 3 || (len(s) > 1 && s[1] == ':') {
			return unsupported(path)
		}
		if len(parts) == 3 && parts[2] != "ro" && parts[2] != "rw" {
			return unsupported(path)
		}
		m, err := format.ParseVolume(s)
		if err != nil {
			return invalid(path, err.Error())
		}
		kind, src, target = m.Type, m.Source, m.Target
	} else {
		m, err := fields(value, path, "type", "source", "target", "read_only", "bind")
		if err != nil {
			return err
		}
		kind, err = stringValue(m["type"], path+".type")
		if err != nil {
			return err
		}
		target, err = stringValue(m["target"], path+".target")
		if err != nil {
			return err
		}
		if s, exists := m["source"]; exists {
			src, err = stringValue(s, path+".source")
			if err != nil {
				return err
			}
		}
		if ro, exists := m["read_only"]; exists {
			if _, ok := ro.(bool); !ok {
				return invalid(path, "read_only must be boolean")
			}
		}
		if b, exists := m["bind"]; exists {
			if kind != "bind" {
				return unsupported(path + ".bind")
			}
			bind, err := fields(b, path+".bind", "create_host_path")
			if err != nil {
				return err
			}
			if c, exists := bind["create_host_path"]; exists {
				if _, ok := c.(bool); !ok {
					return invalid(path, "create_host_path must be boolean")
				}
			}
		}
	}
	if kind != "volume" && kind != "bind" {
		return unsupported(path + ".type")
	}
	if !strings.HasPrefix(target, "/") || strings.ContainsAny(target, "\\\x00") {
		return invalid(path, "Mount target must be an absolute Linux path")
	}
	if kind == "bind" {
		return checkPath(src, path+".source")
	}
	if src != "" && !profilePattern.MatchString(src) {
		return invalid(path, "Invalid named volume")
	}
	return nil
}

func admit(cfg map[string]any, interpolated bool) error {
	if _, err := fields(cfg, "", "name", "services", "volumes"); err != nil {
		return err
	}
	if n, ok := cfg["name"]; ok {
		if _, err := stringValue(n, "name"); err != nil {
			return err
		}
	}
	if vs, ok := cfg["volumes"]; ok && vs != nil {
		v, ok := vs.(map[string]any)
		if !ok {
			return invalid("volumes", "Expected mapping")
		}
		for name, options := range v {
			if !profilePattern.MatchString(name) {
				return invalid("volumes."+name, "Invalid volume name")
			}
			if options != nil {
				if _, err := fields(options, "volumes."+name); err != nil {
					return err
				}
			}
		}
	}
	raw, exists := cfg["services"]
	if !exists {
		return nil
	}
	services, ok := raw.(map[string]any)
	if !ok {
		return invalid("services", "Expected services mapping")
	}
	for name, v := range services {
		path := "services." + name
		if !regexp.MustCompile(`^[a-zA-Z0-9._-]+$`).MatchString(name) {
			return invalid(path, "Invalid service name")
		}
		s, err := fields(v, path, "image", "command", "entrypoint", "environment", "labels", "ports", "volumes", "depends_on", "profiles")
		if err != nil {
			return err
		}
		for k, value := range s {
			p := path + "." + k
			switch k {
			case "image":
				if _, err := stringValue(value, p); err != nil {
					return err
				}
			case "command", "entrypoint":
				if _, ok := value.(string); ok {
					return unsupported(p)
				}
				if value != nil {
					if err := stringsOnly(value, p); err != nil {
						return err
					}
				}
			case "environment", "labels":
				if value == nil {
					continue
				}
				switch m := value.(type) {
				case map[string]any:
					for _, e := range m {
						switch e.(type) {
						case nil, string, bool, int, int64, uint64, float64:
						default:
							return invalid(p, "Expected scalar map values")
						}
					}
				case []any:
					if err := stringsOnly(m, p); err != nil {
						return err
					}
				default:
					return invalid(p, "Expected mapping or string array")
				}
			case "profiles":
				if err := stringsOnly(value, p); err != nil {
					return err
				}
				if interpolated {
					for _, profile := range value.([]any) {
						if !profilePattern.MatchString(profile.(string)) {
							return invalid(p, "Invalid service profile")
						}
					}
				}
			case "depends_on":
				if value == nil {
					continue
				}
				if deps, ok := value.(map[string]any); ok {
					for dep, opts := range deps {
						m, err := fields(opts, p+"."+dep, "condition", "required", "restart")
						if err != nil {
							return err
						}
						if interpolated {
							for k, expected := range map[string]any{"condition": "service_started", "required": true, "restart": false} {
								if actual, exists := m[k]; exists && actual != expected {
									return unsupported(p + "." + dep + "." + k)
								}
							}
						}
					}
				} else if err := stringsOnly(value, p); err != nil {
					return err
				}
			case "ports", "volumes":
				if value == nil {
					continue
				}
				a, ok := value.([]any)
				if !ok {
					return invalid(p, "Expected array")
				}
				for _, item := range a {
					if !interpolated {
						if m, ok := item.(map[string]any); ok {
							if k == "ports" {
								_, err = fields(m, p, "target", "published", "host_ip", "protocol")
							} else {
								_, err = fields(m, p, "type", "source", "target", "read_only", "bind")
								if err == nil {
									if b, exists := m["bind"]; exists {
										_, err = fields(b, p+".bind", "create_host_path")
									}
								}
							}
						}
					} else if k == "ports" {
						err = admitPort(item, p)
					} else {
						err = admitMount(item, p)
					}
					if err != nil {
						return err
					}
				}
			}
		}
	}
	return nil
}
