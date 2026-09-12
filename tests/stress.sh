#!/bin/sh
set -eu

name="/cht_stress_$$"
cleanup() {
    if [ "${server_pid:-}" ]; then
        kill -TERM "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

./server 31 8 "$name" >build/stress-server.log 2>&1 &
server_pid=$!
sleep 1
client_pids=""
for worker in 0 1 2 3; do
    (
        i=0
        while [ "$i" -lt 200 ]; do
            value=$((worker * 1000 + i))
            echo "put $value"
            echo "exists $value"
            echo "delete $((worker * 1000 + i - 1))"
            i=$((i + 1))
        done
        echo quit
    ) | ./client "$name" >/dev/null &
    client_pids="$client_pids $!"
done
for client_pid in $client_pids; do
    wait "$client_pid"
done
kill -TERM "$server_pid"
wait "$server_pid" || true
unset server_pid
echo "stress: completed 2,400 concurrent requests"
