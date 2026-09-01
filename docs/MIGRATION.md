# Test-board application migration

The test-board application now runs exclusively through the CMPP runtime. The
old product-specific Light MCP executor and WS2812 driver have been removed;
there is no second actuator path alongside Core.

The application assembly is intentionally split into three layers:

1. `board_config.c` owns GPIO numbers, polarity, LEDC configuration and safe
   startup levels.
2. `caelus_device_function` owns the product-facing PWM and relay capability
   contracts, validation, execution, observation and safe-stop behavior.
3. `cmpp_device_server.c` attaches the generic MCP adapter to the authenticated
   HTTP transport and advertises `_cmpp._tcp`.

This separation keeps the generic SDK independent of this board. A later board
revision can replace the assembly configuration, while a different product can
provide another Function Module without changing Core, the JSON codec or the
MCP adapter.

Before treating the board as production hardware, validate cold boot and
download mode with the real external loads on ESP32-C3 strapping pins GPIO2,
GPIO8 and GPIO9. Also verify all relay outputs remain off during reset and
brownout. Those pre-boot electrical properties cannot be guaranteed by the
firmware's runtime initialization.
