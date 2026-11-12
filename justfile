set shell := ["bash", "-euo", "pipefail", "-c"]
default:
    @just --list
build distro="fedora":
    ./packaging/container-build.sh {{distro}}
check distro="fedora": guard
    ./packaging/container-build.sh {{distro}}
guard:
    bash tests/dbus_contract_check.sh
    python3 tests/guards.py
docs:
    python3 tests/docs_check.py
    groff -man -Tutf8 -z man/dwarfsctl.1
