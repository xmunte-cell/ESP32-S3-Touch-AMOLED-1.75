idf_component_register(
    SRCS "AgendaApp.cpp" "AgendaIcon.cpp"
    INCLUDE_DIRS ${CMAKE_CURRENT_LIST_DIR}
    REQUIRES brookesia_core
    PRIV_REQUIRES jarvis_config jarvis_fonts
)

set_source_files_properties(
    "AgendaApp.cpp"
    "AgendaIcon.cpp"
    PROPERTIES COMPILE_FLAGS "-Wno-missing-field-initializers"
)
