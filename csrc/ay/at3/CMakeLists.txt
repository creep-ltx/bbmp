# SPDX-License-Identifier: GPL-3.0-or-later
# bbmp-ay-at3, built as one more command line tool inside Arkos Tracker 3's
# source tree (tools/build-ay.sh copies this folder to
# source/commandLineTools/modlandAt3).
cmake_minimum_required(VERSION 3.12)

project(ModlandAt3 VERSION 1.0.0)

juce_add_console_app(ModlandAt3 PRODUCT_NAME "bbmp-ay-at3")

target_sources(ModlandAt3 PRIVATE bbmp-ay-at3.cpp)

target_link_libraries(ModlandAt3
        PRIVATE
        BaseExport
        juce::juce_audio_basics
        PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags)
