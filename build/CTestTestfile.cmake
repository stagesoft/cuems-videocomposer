# CMake generated Testfile for 
# Source directory: /home/ion/src/cuems/cuems-videocomposer
# Build directory: /home/ion/src/cuems/cuems-videocomposer/build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(cuems_videocomposer_unit_tests "/home/ion/src/cuems/cuems-videocomposer/build/cuems_videocomposer_test")
set_tests_properties(cuems_videocomposer_unit_tests PROPERTIES  _BACKTRACE_TRIPLES "/home/ion/src/cuems/cuems-videocomposer/CMakeLists.txt;520;add_test;/home/ion/src/cuems/cuems-videocomposer/CMakeLists.txt;0;")
subdirs("src/cuemslogger")
subdirs("src/mtcreceiver")
