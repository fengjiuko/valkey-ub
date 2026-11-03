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

    test {INFO urma reports disabled state without touching hardware} {
        set info [r info urma]
        assert_match "*# URMA\r\n*" $info
        assert {[regexp {urma_compiled:[01]\r\n} $info]}
        foreach field {urma_listener_enabled urma_buffer_used urma_transports urma_retired_transports urma_ready_total urma_tx_completions urma_tx_wire_bytes urma_rx_frames urma_rx_wire_bytes urma_errors urma_progress_timeouts} {
            assert_match "*${field}:0\r\n*" $info
        }
        assert_match "*urma_buffer_limit:67108864\r\n*" $info
        assert_match "*urma_progress_timeout_ms:10000\r\n*" $info
    }

    test {INFO all includes the native transport section} {
        assert_match "*# URMA\r\n*" [r info all]
    }

    test {Ordinary connections continue to use RESP when URMA is disabled} {
        assert_equal PONG [r ping]
    }
}
