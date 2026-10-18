setup { CREATE EXTENSION test_dsg_region; }
teardown { DROP EXTENSION test_dsg_region; }

session "writer"
step "write_first" { SELECT test_dsg_shared_write(17); }
step "read_second" { SELECT test_dsg_shared_read() = 34 AS shared; }

session "reader"
step "read_first" { SELECT test_dsg_shared_read() = 17 AS shared; }
step "write_second" { SELECT test_dsg_shared_write(34); }

permutation "write_first" "read_first" "write_second" "read_second"
