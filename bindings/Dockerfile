# Binding build environment for the PySide6/Shiboken6 extension (PLAN.md Phase B).
#
# Why a container at all: the pip shiboken6_generator binary needs libxml2.so.2,
# which Arch no longer ships (Arch moved to libxml2.so.16 with libxml2 2.14; the
# soname change is not ABI-safe, do not symlink it for a real build). RHEL9 - the
# base behind PySide6's own manylinux_2_34 wheels - has libxml2.so.2, and the
# wheel does not bundle libclang, so clang-libs is needed too.
#
# RHEL9 has no Qt6 dev packages, and the PySide6 wheel ships no Qt headers, no
# moc and no Qt6Config.cmake (see AGENTS.md), so a real Qt 6.12.0 - the exact
# version PySide6 6.12.0 bundles - is unpacked under /opt/Qt.
#
# aqtinstall is deliberately NOT used: with Qt >= 6.9 the Linux desktop build is
# a single package (qt.qt6.6120.linux_gcc_64) whose archives live in that
# package's own directory, and aqt 3.3.0 cannot parse it ("The packages
# ['qt_base'] were not found"). The two archives we actually need are downloaded
# directly instead - pinned by version string and checked against the mirror's
# .sha1. Qt 6.12's Linux build being RHEL 9 based also matches manylinux_2_34.
#
# patchelf is deliberately absent: it is not in the RHEL9 base repos. Phase F
# (auditwheel/cibuildwheel) gets it from the PyPI patchelf wheel instead.
# libicu-devel is deliberately absent too: Qt wants ICU 73 (bundled in the Qt
# archive, and the PySide6 wheel bundles the same 73), while RHEL9 ships ICU 67 -
# installing -devel would let find_package(ICU) record libicu*.so.67 in the link
# interface, which nothing would satisfy at runtime.
FROM rockylinux:9

RUN dnf -y install \
        python3.11 python3.11-pip python3.11-devel \
        clang-libs libxslt \
        gcc gcc-c++ make which findutils \
        mesa-libGL mesa-libEGL libxkbcommon libdrm fontconfig freetype dbus-libs \
        libglvnd-devel \
        glib2 zlib libzstd \
    && dnf clean all

ENV VIRTUAL_ENV=/opt/venv
RUN python3.11 -m venv /opt/venv && \
    /opt/venv/bin/pip install --no-cache-dir --upgrade pip && \
    /opt/venv/bin/pip install --no-cache-dir \
        "PySide6-Essentials==6.12.0" \
        "shiboken6_generator==6.12.0" \
        py7zr scikit-build-core ninja cmake

# Qt 6.12.0 headers, moc, CMake configs and runtime libs.
# mirror.yandex.ru/mirrors/qt.io measured 0.32s from inside the container
# (ftp.fau.de/qtproject 0.40s also works; download.qt.io times out here).
RUN set -eux; \
    mkdir -p /opt/Qt/6.12.0/gcc_64/lib; \
    cd /tmp; \
    base=https://mirror.yandex.ru/mirrors/qt.io/online/qtsdkrepository/linux_x64/desktop/qt6_6120/qt6_6120/qt.qt6.6120.linux_gcc_64; \
    rev=6.12.0-0-202609280346; \
    for a in qtbase-Linux-RHEL_9_6-GCC-Linux-RHEL_9_6-X86_64 icu-linux-Rhel8.6-x86_64; do \
        curl -fL -o "$a.7z" "$base/$rev$a.7z"; \
        curl -fsSL -o "$a.7z.sha1" "$base/$rev$a.7z.sha1"; \
        echo "$(cat "$a.7z.sha1")  $a.7z" | sha1sum --check; \
    done; \
    /opt/venv/bin/python -m py7zr x qtbase-Linux-RHEL_9_6-GCC-Linux-RHEL_9_6-X86_64.7z \
        /opt/Qt/6.12.0/gcc_64; \
    /opt/venv/bin/python -m py7zr x icu-linux-Rhel8.6-x86_64.7z /opt/Qt/6.12.0/gcc_64/lib; \
    rm -f /tmp/*.7z /tmp/*.sha1

ENV QTDIR=/opt/Qt/6.12.0/gcc_64
# The wheel's libclang is statically linked but has no built-in resource dir of
# its own; clang-libs provides /usr/lib/clang/<ver>/include (stddef.h etc.), and
# CLANG_INSTALL_DIR is the first location the generator checks.
ENV CLANG_INSTALL_DIR=/usr
ENV PATH=/opt/venv/bin:$QTDIR/bin:$PATH

WORKDIR /work
