v_cc_library(
  NAME
    storage_opfuzz
  SRCS
    opfuzz.cc
  DEPS
    v::storage
    v::storage_test_utils
)
rp_test(
  UNIT_TEST
  BINARY_NAME opfuzz_test
  SOURCES opfuzz_test.cc
  LIBRARIES v::seastar_testing_main v::storage_opfuzz
  ARGS "-- -c 1"
  LABELS storage
  )
