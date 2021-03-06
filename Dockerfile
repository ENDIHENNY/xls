# Download base image ubuntu 20.04
FROM ubuntu:20.04

# LABEL about the custom image
LABEL version="0.1"
LABEL description="Docker Image for Building/Testing XLS"

# Update package info
RUN apt-get update -y

# Install Bazel
RUN apt-get install -y curl gnupg && \
curl -fsSL https://bazel.build/bazel-release.pub.gpg | gpg --dearmor > bazel.gpg && \
mv bazel.gpg /etc/apt/trusted.gpg.d/ && \
echo "deb [arch=amd64] https://storage.googleapis.com/bazel-apt stable jdk1.8" | tee /etc/apt/sources.list.d/bazel.list && \
apt-get update -y && apt-get install -y bazel

# Install dependencies
RUN apt-get -y install python3-distutils python3-dev python-is-python3 libtinfo5

# Install development tools
RUN apt-get install -y git vim

RUN useradd -m xls-developer
USER xls-developer

# Clone the project
WORKDIR /home/xls-developer/
RUN git clone https://github.com/google/xls.git

WORKDIR /home/xls-developer/xls

# Build everything (opt)
RUN bazel build -c opt ...

# Test everything (opt)
RUN bazel test -c opt ...
