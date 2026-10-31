// Copyright (C) Microsoft Corporation. All rights reserved.
package main

import (
	"context"
	"path/filepath"
	"sort"
	"strconv"

	"github.com/compose-spec/compose-go/v2/interpolation"
	"github.com/compose-spec/compose-go/v2/loader"
	"github.com/compose-spec/compose-go/v2/types"
	"go.yaml.in/yaml/v4"
)

func normalize(ctx context.Context, r request) result {
	env := types.Mapping{}
	for k, v := range r.Environment {
		env[k] = v
	}
	env["COMPOSE_PROJECT_NAME"] = r.Name
	details := types.ConfigDetails{WorkingDir: r.Directory, Environment: env}
	configure := func(o *loader.Options) {
		o.SetProjectName(r.Name, true)
		o.SetWorkingDirExplicit(true)
		o.SkipInclude = true
		o.SkipExtends = true
		o.SkipResolveEnvironment = true
		o.SkipResolveLabels = true
		o.Profiles = []string{"*"}
		o.MaxNodeVisits = maxNodes
		o.Interpolate.LookupValue = func(k string) (string, bool) { v, ok := env[k]; return v, ok }
	}
	options := loader.ToOptions(&details, []func(*loader.Options){configure})
	reader := yamlReader{active: map[*yaml.Node]bool{}}
	for _, s := range r.Sources {
		cfg, err := reader.read(s.Content)
		if err != nil {
			return errorResult(err, s.ID)
		}
		if err = admit(cfg, false); err != nil {
			return errorResult(err, s.ID)
		}
		cfg, err = interpolation.Interpolate(cfg, *options.Interpolate)
		if err != nil {
			return errorResult(err, s.ID)
		}
		if err = admit(cfg, true); err != nil {
			return errorResult(err, s.ID)
		}
		details.ConfigFiles = append(details.ConfigFiles, types.ConfigFile{Filename: s.Filename, Content: []byte(s.Content), Config: cfg})
	}
	// Captured, admitted, interpolated Config maps bypass file loading. Keep
	// schema/consistency/defaults enabled; no file-backed features are admitted.
	configureLoaded := func(o *loader.Options) { configure(o); o.SkipInterpolation = true }
	model, err := loader.LoadModelWithContext(ctx, details, configureLoaded)
	if err != nil {
		return errorResult(err, "")
	}
	states := map[string]map[string]any{}
	if services, ok := model["services"].(map[string]any); ok {
		for name, value := range services {
			s := value.(map[string]any)
			states[name] = map[string]any{"command": command(s, "command"), "entrypoint": command(s, "entrypoint")}
		}
	}
	project, err := loader.ModelToProject(model, loader.ToOptions(&details, []func(*loader.Options){configureLoaded}), details)
	if err != nil {
		return errorResult(err, "")
	}
	project, err = project.WithProfiles(r.Profiles)
	if err != nil {
		return errorResult(err, "")
	}
	services := map[string]any{}
	for name, s := range project.AllServices() {
		environment := s.Environment.Resolve(func(k string) (string, bool) { v, ok := env[k]; return v, ok }).ToMapping()
		labels := map[string]string{}
		for k, v := range s.Labels {
			labels[k] = v
		}
		ports := []any{}
		for _, p := range s.Ports {
			var published any
			if p.Published != "" {
				n, err := strconv.Atoi(p.Published)
				if err != nil {
					return errorResult(err, "")
				}
				published = n
			}
			ports = append(ports, map[string]any{"target": p.Target, "published": published, "host_ip": p.HostIP, "protocol": p.Protocol})
		}
		mounts := []any{}
		for _, v := range s.Volumes {
			create := v.Bind != nil && v.Bind.CreateHostPath
			src := v.Source
			if v.Type == "bind" {
				src = filepath.Clean(src)
			}
			mounts = append(mounts, map[string]any{"type": v.Type, "source": src, "target": v.Target, "read_only": v.ReadOnly, "create_host_path": create})
		}
		deps := []string{}
		_, enabled := project.Services[name]
		for dep := range s.DependsOn {
			deps = append(deps, dep)
			if _, available := project.Services[dep]; enabled && !available {
				return errorResult(invalid("services."+name+".depends_on", "Enabled service depends on a disabled service"), "")
			}
		}
		sort.Strings(deps)
		profiles := append([]string{}, s.Profiles...)
		services[name] = map[string]any{
			"image": s.Image, "command": states[name]["command"], "entrypoint": states[name]["entrypoint"],
			"environment": environment, "labels": labels, "ports": ports, "mounts": mounts,
			"dependencies": deps, "profiles": profiles, "enabled": enabled,
		}
	}
	volumes := map[string]string{}
	for name, v := range project.Volumes {
		volumes[name] = v.Name
	}
	network := ""
	if n, exists := project.Networks["default"]; exists {
		network = n.Name
	}
	return result{1, "success", []diagnostic{}, map[string]any{
		"name": project.Name, "directory": filepath.Clean(r.Directory), "services": services,
		"volumes": volumes, "default_network_name": network,
	}}
}

func command(s map[string]any, key string) map[string]any {
	state := "absent"
	args := []any{}
	if v, exists := s[key]; exists {
		if v == nil {
			state = "null"
		} else {
			state = "value"
			args = v.([]any)
		}
	}
	return map[string]any{"state": state, "arguments": args}
}
