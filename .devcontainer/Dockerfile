# [Choice] Ubuntu version (use jammy or bionic on local arm64/Apple Silicon): jammy, focal, bionic
ARG VARIANT="jammy"
FROM buildpack-deps:${VARIANT}-curl

LABEL dev.containers.features="common"

RUN echo 'deb-src http://de.archive.ubuntu.com/ubuntu/ jammy main restricted universe multiverse' >> /etc/apt/sources.list

RUN apt-get update && export DEBIAN_FRONTEND=noninteractive \
    && apt-get -y build-dep gnumeric && apt-get -y install gtk-doc-tools yelp yelp-tools
