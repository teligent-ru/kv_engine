ADD_LIBRARY(mcutils STATIC
            hostname_utils.cc hostname_utils.h)
TARGET_LINK_LIBRARIES(mcutils mcbp cbsasl ${COUCHBASE_NETWORK_LIBS})

ADD_EXECUTABLE(memcached-hostutils-test
               hostname_utils_test.cc)
TARGET_LINK_LIBRARIES(memcached-hostutils-test
                      mcutils
                      platform
                      gtest
                      gtest_main)
ADD_TEST(NAME memcached-hostutils-test
         WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
         COMMAND memcached-hostutils-test)

ADD_LIBRARY(getpass STATIC
            getpass.cc getpass.h)

ADD_SUBDIRECTORY(engine_testapp)
ADD_SUBDIRECTORY(mcctl)
ADD_SUBDIRECTORY(mclogsplit)
ADD_SUBDIRECTORY(mcstat)
ADD_SUBDIRECTORY(mctimings)
ADD_SUBDIRECTORY(moxi_hammer)
