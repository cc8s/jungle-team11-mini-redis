FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends         build-essential gdb valgrind make git curl ca-certificates         python3 python3-pip python3-venv redis-tools     && rm -rf /var/lib/apt/lists/*
