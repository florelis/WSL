# Copyright (C) Microsoft Corporation. All rights reserved.
add_executable(compose_fake_worker FakeWorker.cpp)
target_link_libraries(compose_fake_worker PRIVATE compose_parser)
target_precompile_headers(compose_fake_worker PRIVATE precomp.h)
set_target_properties(compose_fake_worker PROPERTIES FOLDER tests)
add_dependencies(wsltests compose_fake_worker)
