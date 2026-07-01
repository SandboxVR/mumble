# Copyright 2023 The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

add_executable(TestDuplicateAudioCorrelator TestDuplicateAudioCorrelator.cpp)

set_target_properties(TestDuplicateAudioCorrelator PROPERTIES AUTOMOC ON)

target_sources(TestDuplicateAudioCorrelator PRIVATE
	"${CMAKE_SOURCE_DIR}/src/murmur/DuplicateAudioCorrelator.cpp"
)

target_include_directories(TestDuplicateAudioCorrelator PRIVATE
	"${CMAKE_SOURCE_DIR}/src/murmur"
)

find_pkg("opus;Opus" REQUIRED)
target_include_directories(TestDuplicateAudioCorrelator PRIVATE ${opus_INCLUDE_DIRS})
target_link_libraries(TestDuplicateAudioCorrelator PRIVATE shared Qt5::Test ${opus_LIBRARIES})
if(TARGET opus)
	target_link_libraries(TestDuplicateAudioCorrelator PRIVATE opus)
elseif(TARGET Opus)
	target_link_libraries(TestDuplicateAudioCorrelator PRIVATE Opus)
elseif(TARGET Opus::opus)
	target_link_libraries(TestDuplicateAudioCorrelator PRIVATE Opus::opus)
endif()

add_test(NAME TestDuplicateAudioCorrelator COMMAND $<TARGET_FILE:TestDuplicateAudioCorrelator>)
