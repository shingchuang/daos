#!/bin/bash
## Run linters across control plane code and execute Go tests
set -eu

function find_build_source()
{
	BASE=$PWD
	while true
	do
		path=$(realpath "${BASE}")
		if [ "${path}" == "/" ]; then
			break
		fi
		bvp="${path}/.build_vars.sh"
		test -e "${bvp}" && echo "${bvp}" && return
		BASE=$(dirname "${path}")
	done
	echo ""
}

function check_environment()
{
	if [ -z "${LD_LIBRARY_PATH:-}" ]; then
		echo "false" && return
	fi

	if [ -z "${CGO_LDFLAGS:-}" ]; then
		echo "false" && return
	fi

	if [ -z "${CGO_CFLAGS:-}" ]; then
		echo "false" && return
	fi
	echo "true" && return
}

function setup_environment()
{
	build_source=$(find_build_source)

	if [ "${build_source}" == "" ]; then
		echo "Unable to find .build_vars.sh" && exit 1
	fi

	source "${build_source}"

	# allow cgo to find and link to third-party libs
	LD_LIBRARY_PATH=${SL_PREFIX}/lib64
	LD_LIBRARY_PATH+=":${SL_SPDK_PREFIX}/lib"
	LD_LIBRARY_PATH+=":${SL_OFI_PREFIX}/lib"
	LD_LIBRARY_PATH+=":${SL_ISAL_PREFIX}/lib"
	CGO_LDFLAGS=-L${SL_PREFIX}/lib64
	CGO_LDFLAGS+=" -L${SL_SPDK_PREFIX}/lib"
	CGO_LDFLAGS+=" -L${SL_OFI_PREFIX}/lib"
	CGO_LDFLAGS+=" -L${SL_ISAL_PREFIX}/lib"
	CGO_LDFLAGS+=" -lisal"
	CGO_CFLAGS=-I${SL_PREFIX}/include
	CGO_CFLAGS+=" -I${SL_SPDK_PREFIX}/include"
	CGO_CFLAGS+=" -I${SL_OFI_PREFIX}/include"
	CGO_CFLAGS+=" -I${SL_ISAL_PREFIX}/include"
}

function check_formatting()
{
	srcdir=${1:-"./"}
	output=$(find "$srcdir/" -name '*.go' -and -not -path '*vendor*' \
		-print0 | xargs -0 gofmt -d)
	if [ -n "$output" ]; then
		echo "ERROR: Your code hasn't been run through gofmt!"
		echo "Please configure your editor to run gofmt on save."
		echo "Alternatively, at a minimum, run the following command:"
		echo -n "find $srcdir/ -name '*.go' -and -not -path '*vendor*'"
		echo "| xargs gofmt -w"
		echo -e "\ngofmt check found the following:\n\n$output\n"
		exit 1
	fi
}

function get_test_runner()
{
	test_args="-race -cover -v ./..."
	test_runner="go test"

	if which gotestsum >/dev/null; then
		mkdir -p "$(dirname "$GO_TEST_XML")"
		test_runner="gotestsum --format short "
		test_runner+="--junitfile-testcase-classname relative "
		test_runner+="--junitfile-testsuite-name relative "
		if [ -n "${IS_CI:-}" ]; then
			test_runner+="--no-color "
		fi
		test_runner+="--junitfile $GO_TEST_XML --"
	fi

	echo "$test_runner $test_args"
}

check=$(check_environment)

if [ "$check" == "false" ]; then
	setup_environment
fi

DAOS_BASE=${DAOS_BASE:-${SL_PREFIX%/install*}}
export PATH=$SL_PREFIX/bin:$PATH
GO_TEST_XML="$DAOS_BASE/test_results/run_go_tests.xml"
GO_TEST_RUNNER=$(get_test_runner)

DIR="$(readlink -f "$(dirname "${BASH_SOURCE[0]}")")"
GOPATH="$(readlink -f "$DIR/../../build/src/control")"
repopath=github.com/daos-stack/daos
controldir="$GOPATH/src/$repopath/src/control"

check_formatting "$controldir"

echo "Environment:"
echo "  GO VERSION: $(go version | awk '{print $3" "$4}')"
echo "  LD_LIBRARY_PATH: $LD_LIBRARY_PATH"
echo "  CGO_LDFLAGS: $CGO_LDFLAGS"
echo "  CGO_CFLAGS: $CGO_CFLAGS"

echo "  GOPATH: $GOPATH"
echo

echo "Running all tests under $controldir..."
pushd "$controldir" >/dev/null
set +e
LD_LIBRARY_PATH="$LD_LIBRARY_PATH" \
CGO_LDFLAGS="$CGO_LDFLAGS" \
CGO_CFLAGS="$CGO_CFLAGS" \
GOPATH="$GOPATH" \
	$GO_TEST_RUNNER
testrc=$?
popd >/dev/null

if [ -f "$GO_TEST_XML" ]; then
	# add a newline to make the XML parser happy
	echo >> "$GO_TEST_XML"
fi

echo "Tests completed with rc: $testrc"
exit $testrc
