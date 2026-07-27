# [Choice] Ubuntu version: noble (24.04 LTS), jammy (22.04 LTS)
ARG VARIANT="noble"
# -scm rather than -curl: git is needed to fetch goffice below.
FROM buildpack-deps:${VARIANT}-scm

LABEL dev.containers.features="common"

# Source repositories are needed for 'apt-get build-dep'. Noble and later use
# the deb822 format; older releases use the legacy sources.list.
RUN if [ -f /etc/apt/sources.list.d/ubuntu.sources ]; then \
        sed -i 's/^Types: deb$/Types: deb deb-src/' /etc/apt/sources.list.d/ubuntu.sources; \
    else \
        sed -i 's/^# *deb-src/deb-src/' /etc/apt/sources.list; \
    fi

RUN apt-get update && export DEBIAN_FRONTEND=noninteractive \
    && apt-get -y build-dep goffice gnumeric \
    && apt-get -y install gtk-doc-tools yelp yelp-tools \
    && rm -rf /var/lib/apt/lists/*

# Ubuntu ships at most goffice 0.10.57, but gnumeric master requires
# >= 0.10.61, so build it from source into /usr/local.
RUN git clone --depth 1 https://gitlab.gnome.org/GNOME/goffice.git /tmp/goffice \
    && cd /tmp/goffice \
    && NOCONFIGURE=1 ./autogen.sh \
    && ./configure --prefix=/usr/local \
    && make -j"$(nproc)" \
    && make install \
    && ldconfig \
    && rm -rf /tmp/goffice

ENV PKG_CONFIG_PATH=/usr/local/lib/pkgconfig
