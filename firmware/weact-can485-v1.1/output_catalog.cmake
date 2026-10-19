# Output catalog inputs for the WeAct CAN485 V1.1 build. The firmware project
# and the host build both include this file, so host tests compile the same
# generated limits header as the firmware.
set(OUTPUT_CATALOG_DESCRIPTORS
    "${CMAKE_CURRENT_LIST_DIR}/../../components/local_argb_actions/output_catalog.yaml")
set(OUTPUT_CATALOG_BOARD
    "strip_max_pixels=300")
