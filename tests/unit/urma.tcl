start_server {tags {"urma"}} {
    test {URMA defaults to disabled with a bounded buffer budget} {
        assert_equal 0 [lindex [r config get urma-port] 1]
        assert_equal no [lindex [r config get urma-allow-insecure-bootstrap] 1]
        assert_equal 67108864 [lindex [r config get urma-buffer-limit] 1]
        assert_equal 10000 [lindex [r config get urma-progress-timeout] 1]
    }

    test {URMA startup configuration cannot be changed at runtime} {
        foreach {name value} {
            urma-port 6380
            urma-eid-index 1
            urma-device test-device
            urma-buffer-limit 1
            urma-progress-timeout 20000
            urma-allow-insecure-bootstrap yes
        } {
            assert_error {*immutable*} {r config set $name $value}
        }
    }

    test {Ordinary connections continue to use RESP when URMA is disabled} {
        assert_equal PONG [r ping]
    }
}
