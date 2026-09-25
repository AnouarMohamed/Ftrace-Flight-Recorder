FROM gcr.io/oss-fuzz-base/base-builder@sha256:b1f0fce811b848e117a4809f8ccf1bdf94d1b5d5406d19e8512ac3f0ce958b3c
RUN apt-get update && apt-get install -y make
WORKDIR /src/Ftrace-Flight-Recorder
COPY . .
