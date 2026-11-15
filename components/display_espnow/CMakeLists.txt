# Round display ESP-NOW link (docs/SPEC.md, pairing v1 responder and
# telemetry v1 sender). sdk/ and include/pairing/ are copied byte for byte
# from the esp32-1-75 display project's reviewed esp-now 2.5.3 adaptation;
# the firmware CMakeLists applies sdk/overlay.json before compilation.
idf_component_register(INCLUDE_DIRS "include"
                       REQUIRES esp-now)
