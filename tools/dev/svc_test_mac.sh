root=$PWD; here=$root/tools/linux-check; out=$TMPDIR/tu
for n in "$@"; do
inc=$(grep -o '"\.\./src/services/[^"]*\.c"' runtime/tests/test_svc_$n.c | sed 's#.*/##;s#"##')
srcs=$(ls runtime/src/services/*_core.c | grep -v "/$inc\$")
cc -std=gnu11 -w -D_FORTIFY_SOURCE=0 -include $here/compat.h -I$here/include -I$root/runtime/src -I$root/runtime/src/services -I$root/runtime/tests runtime/tests/test_svc_$n.c $srcs $here/ipc_mock.c runtime/src/services/host_info.c runtime/src/json.c -framework Foundation -lm -lpthread -o $out/t_$n 2>&1 | grep -E "error|\"_" | head -5
echo "-- $n"; $out/t_$n 2>&1 | tail -3
done
