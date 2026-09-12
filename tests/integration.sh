#!/bin/sh
set -eu

name="/cht_test_$$"
server_log="build/integration-server.log"
cleanup() {
    if [ "${server_pid:-}" ]; then
        kill -TERM "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

./server 2 4 "$name" >"$server_log" 2>&1 &
server_pid=$!
sleep 1

output=$(printf 'put -7\nput -7\nexists -7\nexists 999\ndelete 999\ntest-get 0\ntest-get 1\nquit\n' | ./client "$name")
printf '%s\n' "$output" | grep -q 'cht> success'
printf '%s\n' "$output" | grep -q 'cht> exists'
printf '%s\n' "$output" | grep -q 'cht> not found'
printf '%s\n' "$output" | grep -q 'bucket contains 1 item(s)'

(for n in 1 2 3 4 5 6 7 8; do echo "put $n"; done; echo quit) | ./client "$name" >/dev/null &
client_a=$!
(for n in 9 10 11 12 13 14 15 16; do echo "put $n"; done; echo quit) | ./client "$name" >/dev/null &
client_b=$!
wait "$client_a"
wait "$client_b"

output=$(printf 'test-get 0\ntest-get 1\nquit\n' | ./client "$name")
printf '%s\n' "$output" | grep -q 'bucket contains'
kill -TERM "$server_pid"
wait "$server_pid" || true
unset server_pid
echo "integration: passed"
