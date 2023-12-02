PY3TEST()

PEERDIR(
    contrib/python/pyasn1-modules
)

TEST_SRCS(
    __init__.py
    test_missing.py
    test_pem.py
    test_rfc2314.py
    test_rfc2315.py
    test_rfc2437.py
    test_rfc2459.py
    test_rfc2511.py
    test_rfc2560.py
    test_rfc2631.py
    test_rfc2634.py
    test_rfc2876.py
    test_rfc2985.py
    test_rfc2986.py
    test_rfc3058.py
    test_rfc3114.py
    test_rfc3125.py
    test_rfc3161.py
    test_rfc3274.py
    test_rfc3279.py
    test_rfc3280.py
    test_rfc3281.py
    test_rfc3370.py
    test_rfc3447.py
    test_rfc3537.py
    test_rfc3560.py
    test_rfc3565.py
    test_rfc3657.py
    test_rfc3709.py
    test_rfc3739.py
    test_rfc3770.py
    test_rfc3779.py
    test_rfc3820.py
    test_rfc3852.py
    test_rfc4010.py
    test_rfc4043.py
    test_rfc4055.py
    test_rfc4073.py
    test_rfc4108.py
    test_rfc4210.py
    test_rfc4211.py
    test_rfc4334.py
    test_rfc4357.py
    test_rfc4387.py
    test_rfc4476.py
    test_rfc4490.py
    test_rfc4491.py
    test_rfc4683.py
    test_rfc4985.py
    test_rfc5035.py
    test_rfc5083.py
    test_rfc5084.py
    test_rfc5126.py
    test_rfc5208.py
    test_rfc5275.py
    test_rfc5280.py
    test_rfc5480.py
    test_rfc5636.py
    test_rfc5639.py
    test_rfc5649.py
    test_rfc5652.py
    test_rfc5697.py
    test_rfc5751.py
    test_rfc5752.py
    test_rfc5753.py
    test_rfc5755.py
    test_rfc5913.py
    test_rfc5914.py
    test_rfc5915.py
    test_rfc5916.py
    test_rfc5917.py
    test_rfc5924.py
    test_rfc5934.py
    test_rfc5940.py
    test_rfc5958.py
    test_rfc5990.py
    test_rfc6010.py
    test_rfc6019.py
    test_rfc6031.py
    test_rfc6032.py
    test_rfc6120.py
    test_rfc6187.py
    test_rfc6210.py
    test_rfc6211.py
    test_rfc6402.py
    test_rfc6482.py
    test_rfc6486.py
    test_rfc6487.py
    test_rfc6664.py
    test_rfc6955.py
    test_rfc6960.py
    test_rfc7030.py
    test_rfc7191.py
    test_rfc7229.py
    test_rfc7292.py
    test_rfc7296.py
    test_rfc7508.py
    test_rfc7585.py
    test_rfc7633.py
    test_rfc7773.py
    test_rfc7894.py
    test_rfc7906.py
    test_rfc7914.py
    test_rfc8017.py
    test_rfc8018.py
    test_rfc8103.py
    test_rfc8209.py
    test_rfc8226.py
    test_rfc8358.py
    test_rfc8360.py
    test_rfc8398.py
    test_rfc8410.py
    test_rfc8418.py
    test_rfc8419.py
    test_rfc8479.py
    test_rfc8494.py
    test_rfc8520.py
    test_rfc8619.py
    test_rfc8649.py
    test_rfc8692.py
    test_rfc8696.py
    test_rfc8702.py
    test_rfc8708.py
    test_rfc8769.py
)

NO_LINT()

END()