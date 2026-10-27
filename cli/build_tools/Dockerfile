ARG MANYLINUX_IMAGE
FROM ${MANYLINUX_IMAGE}
COPY cli /opt/cli/
RUN /opt/python/cp313-cp313/bin/python /opt/cli/build_tools/toolchain.py --directory /opt/cmake
ENV ARKIT_CMAKE_CACHE=/opt/cmake
WORKDIR /work/cli
ENTRYPOINT ["/opt/python/cp313-cp313/bin/python", "/work/cli/build_tools/build.py"]
