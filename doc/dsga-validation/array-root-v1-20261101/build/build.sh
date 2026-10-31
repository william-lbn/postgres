set -Eeuo pipefail
export CCACHE_DIR=/ccache CCACHE_MAXSIZE=1G CCACHE_BASEDIR=/src CCACHE_COMPILERCHECK=content
cd /src
sha256sum -c /report/expected-source-sha256.txt > /evidence/actual-source-check.txt
echo 0 > /evidence/source.exitcode
/usr/bin/gcc-12 --version > /evidence/compiler.txt
python3 --version >> /evidence/compiler.txt
dpkg-query -W liburing-dev liburing2 >> /evidence/compiler.txt
cd /build
CC='ccache /usr/bin/gcc-12' /src/configure --prefix=/opt/pg18 --enable-debug --enable-cassert --enable-injection-points --with-openssl --with-icu --with-liburing --with-extra-version=' (15cacfb3-array-root-v1-TEST_ONLY)' 'CFLAGS=-fsigned-char -O2 -g3' 'ICU_CFLAGS=-I/usr/local/icu/include -DU_STATIC_IMPLEMENTATION' 'ICU_LIBS=-L/usr/local/icu/lib -L/usr/local/icu/lib64 -licui18n -licuuc -licudata -lstdc++ -Wl,-Bdynamic -lm'
mold -run make -j4 -s
make -s install
make -C src/test/modules/test_dsg_region18 -j4 -s install
make -C src/test/modules/test_aio -j4 -s install
make -C src/test/modules/injection_points -j4 -s install
make -C contrib/pg_buffercache -j4 -s install
/opt/pg18/bin/pg_config --configure > /evidence/pg-configure.txt
/opt/pg18/bin/pg_config --pkglibdir > /evidence/pkglibdir.txt
/opt/pg18/bin/postgres --version > /evidence/postgres-version.txt
sha256sum /opt/pg18/bin/postgres "$(/opt/pg18/bin/pg_config --pkglibdir)/test_dsg_region18.so" > /evidence/binary.sha256
ccache --show-stats > /evidence/ccache-stats.txt
