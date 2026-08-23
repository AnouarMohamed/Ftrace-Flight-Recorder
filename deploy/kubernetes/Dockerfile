FROM alpine:3.23 AS build
RUN apk add --no-cache build-base curl
WORKDIR /src
COPY Makefile ./
COPY src ./src
COPY tests ./tests
RUN make check \
    && make clean \
    && make

FROM alpine:3.23
RUN apk add --no-cache curl kmod logrotate \
    && install -d -m 0755 /etc/fdr.d /var/lib/logrotate /var/log/fdr
COPY --from=build /src/fdrd /usr/sbin/fdrd
EXPOSE 9119
STOPSIGNAL SIGTERM
HEALTHCHECK --interval=30s --timeout=3s --start-period=10s --retries=3 \
    CMD curl --fail --silent http://127.0.0.1:9119/healthz >/dev/null || exit 1
ENTRYPOINT ["/usr/sbin/fdrd"]
CMD ["-f", "-a", "0.0.0.0", "-p", "9119", "-d", "/sys/kernel/tracing"]
