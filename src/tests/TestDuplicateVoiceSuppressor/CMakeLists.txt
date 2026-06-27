# Copyright 2023 The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

add_executable(TestDuplicateVoiceSuppressor TestDuplicateVoiceSuppressor.cpp)

set_target_properties(TestDuplicateVoiceSuppressor PROPERTIES AUTOMOC ON)

target_sources(TestDuplicateVoiceSuppressor PRIVATE
	"${CMAKE_SOURCE_DIR}/src/murmur/DuplicateVoiceSuppressor.cpp"
)

target_include_directories(TestDuplicateVoiceSuppressor PRIVATE
	"${CMAKE_SOURCE_DIR}/src/murmur"
)

target_link_libraries(TestDuplicateVoiceSuppressor PRIVATE shared Qt5::Test)

add_test(NAME TestDuplicateVoiceSuppressor COMMAND $<TARGET_FILE:TestDuplicateVoiceSuppressor>)
