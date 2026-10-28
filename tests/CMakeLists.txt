find_package(Qt6 REQUIRED COMPONENTS Test)

foreach(name tst_document tst_editor)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE ot_lib Qt6::Test ot_warnings)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endforeach()
