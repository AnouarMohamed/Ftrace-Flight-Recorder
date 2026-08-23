FROM fedora:43 AS build
RUN dnf install -y gcc make && dnf clean all
WORKDIR /src
COPY Makefile ./
COPY src ./src
RUN make

FROM fedora:43
RUN dnf install -y logrotate kmod && dnf clean all
COPY --from=build /src/fdrd /usr/sbin/fdrd
ENTRYPOINT ["/usr/sbin/fdrd"]
CMD ["-f", "-d", "/sys/kernel/tracing"]
