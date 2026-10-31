// Copyright (C) Microsoft Corporation. All rights reserved.
package main

/*
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

enum {
    COMPOSE_ABI_VERSION = 1,
    COMPOSE_SUCCESS = 0,
    COMPOSE_REJECTED = 2,
    COMPOSE_INTERNAL_ERROR = 3,
    COMPOSE_INVALID_ARGUMENT = 4
};
*/
import "C"

import "unsafe"

//export ComposeAbiVersion
func ComposeAbiVersion() C.uint32_t {
	return C.COMPOSE_ABI_VERSION
}

//export ComposeNormalize
func ComposeNormalize(input *C.uint8_t, length C.size_t, output **C.uint8_t, outputLength *C.size_t) (status C.uint32_t) {
	status = C.COMPOSE_INTERNAL_ERROR
	defer func() {
		if recover() != nil {
			status = C.COMPOSE_INTERNAL_ERROR
		}
	}()
	if output != nil {
		*output = nil
	}
	if outputLength != nil {
		*outputLength = 0
	}
	if output == nil || outputLength == nil || (input == nil && length != 0) {
		return C.COMPOSE_INVALID_ARGUMENT
	}

	var response result
	if length > maxRequest {
		response = failure("limit_exceeded", "Request exceeds 4 MiB", "", "")
	} else {
		// Copy before returning; no Go pointer or caller-owned buffer is retained.
		response = interpret(C.GoBytes(unsafe.Pointer(input), C.int(length)))
	}
	encoded, success := encodeResponse(response)
	if encoded == nil {
		return C.COMPOSE_INTERNAL_ERROR
	}
	*output = (*C.uint8_t)(C.CBytes(encoded))
	*outputLength = C.size_t(len(encoded))
	if success {
		return C.COMPOSE_SUCCESS
	}
	for _, diagnostic := range response.Diagnostics {
		if diagnostic.Code == "internal_error" {
			return C.COMPOSE_INTERNAL_ERROR
		}
	}
	return C.COMPOSE_REJECTED
}

//export ComposeFree
func ComposeFree(output *C.uint8_t) {
	C.free(unsafe.Pointer(output))
}
