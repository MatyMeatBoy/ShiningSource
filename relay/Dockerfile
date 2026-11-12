# ShiningSource relay as a container: docker build -t ss_relay relay && docker run -p 47900:47900/udp ss_relay
FROM alpine:3.20 AS build
RUN apk add --no-cache gcc musl-dev
COPY ss_relay.c /src/ss_relay.c
RUN gcc -O2 -static -o /ss_relay /src/ss_relay.c

FROM scratch
COPY --from=build /ss_relay /ss_relay
USER 65532:65532
EXPOSE 47900/udp
CMD ["/ss_relay", "47900"]
