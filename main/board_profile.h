/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "sdkconfig.h"

#if CONFIG_CMPP_VOICE_BOARD
#define BOARD_PROFILE_ID "caelus.voice-terminal.v1"
#define BOARD_DEVICE_TYPE "cmpp-voice-terminal"
#define BOARD_SERVER_NAME "Caelus CMPP Voice Terminal"
#define BOARD_DISPLAY_NAME_PREFIX "Caelus Voice Terminal"
#define BOARD_SERVER_DESCRIPTION "CMPP voice terminal: single-channel PTT PCM audio capture and playback"
#else
#define BOARD_PROFILE_ID "caelus.test-board.v1"
#define BOARD_DEVICE_TYPE "cmpp-test-board"
#define BOARD_SERVER_NAME "Caelus CMPP Test Board"
#define BOARD_DISPLAY_NAME_PREFIX "Caelus Board"
#define BOARD_SERVER_DESCRIPTION "Six PWM indicators and four relay outputs"
#endif
