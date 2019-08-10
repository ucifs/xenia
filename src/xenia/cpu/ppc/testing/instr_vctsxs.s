# 0 * 2^31
test_vctsxs_1:
  #_ REGISTER_IN v0 [00000000, 00000000, 00000000, 00000000]
  vctsxs v3, v0, 31
  blr
  #_ REGISTER_OUT v0 [00000000, 00000000, 00000000, 00000000]
  #_ REGISTER_OUT v3 [00000000, 00000000, 00000000, 00000000]

# -0 ^ 2^31
test_vctsxs_2:
  #_ REGISTER_IN v0 [80000000, 80000000, 80000000, 80000000]
  vctsxs v3, v0, 31
  blr
  #_ REGISTER_OUT v0 [80000000, 80000000, 80000000, 80000000]
  #_ REGISTER_OUT v3 [00000000, 00000000, 00000000, 00000000]

# smallest positive subnormal * 2^31
test_vctsxs_3:
  #_ REGISTER_IN v0 [00000001, 00000001, 00000001, 00000001]
  vctsxs v3, v0, 31
  blr
  #_ REGISTER_OUT v0 [00000001, 00000001, 00000001, 00000001]
  #_ REGISTER_OUT v3 [00000000, 00000000, 00000000, 00000000]

# largest subnormal * 2^31
test_vctsxs_4:
  #_ REGISTER_IN v0 [007FFFFF, 007FFFFF, 007FFFFF, 007FFFFF]
  vctsxs v3, v0, 31
  blr
  #_ REGISTER_OUT v0 [007FFFFF, 007FFFFF, 007FFFFF, 007FFFFF]
  #_ REGISTER_OUT v3 [00000000, 00000000, 00000000, 00000000]

# +1 * 2^0
test_vctsxs_5:
  #_ REGISTER_IN v0 [3F800000, 3F800000, 3F800000, 3F800000]
  vctsxs v3, v0, 0
  blr
  #_ REGISTER_OUT v0 [3F800000, 3F800000, 3F800000, 3F800000]
  #_ REGISTER_OUT v3 [00000001, 00000001, 00000001, 00000001]

# -1 * 2^0
test_vctsxs_6:
  #_ REGISTER_IN v0 [BF800000, BF800000, BF800000, BF800000]
  vctsxs v3, v0, 0
  blr
  #_ REGISTER_OUT v0 [BF800000, BF800000, BF800000, BF800000]
  #_ REGISTER_OUT v3 [FFFFFFFF, FFFFFFFF, FFFFFFFF, FFFFFFFF]

# 2^31 * 2^0
test_vctsxs_7:
  #_ REGISTER_IN v0 [4F000000, 4F000000, 4F000000, 4F000000]
  vctsxs v3, v0, 0
  blr
  #_ REGISTER_OUT v0 [4F000000, 4F000000, 4F000000, 4F000000]
  #_ REGISTER_OUT v3 [7FFFFFFF, 7FFFFFFF, 7FFFFFFF, 7FFFFFFF]

# +infinity * 2^0
test_vctsxs_8:
  #_ REGISTER_IN v0 [7F800000, 7F800000, 7F800000, 7F800000]
  vctsxs v3, v0, 0
  blr
  #_ REGISTER_OUT v0 [7F800000, 7F800000, 7F800000, 7F800000]
  #_ REGISTER_OUT v3 [7FFFFFFF, 7FFFFFFF, 7FFFFFFF, 7FFFFFFF]

# -infinity * 2^0
test_vctsxs_9:
  #_ REGISTER_IN v0 [FF800000, FF800000, FF800000, FF800000]
  vctsxs v3, v0, 0
  blr
  #_ REGISTER_OUT v0 [FF800000, FF800000, FF800000, FF800000]
  #_ REGISTER_OUT v3 [80000000, 80000000, 80000000, 80000000]