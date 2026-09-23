ARG MANYLINUX_IMAGE
FROM ${MANYLINUX_IMAGE}
COPY . /opt/release-tools/
ENV MHARKIT_TOOLS_CACHE=/opt/mh-arkit-tools
RUN /opt/python/cp313-cp313/bin/python /opt/release-tools/run.py --prepare
ENV MHARKIT_TOOLS_OFFLINE=1
WORKDIR /work/cli
ENTRYPOINT ["/opt/python/cp313-cp313/bin/python", "/work/cli/build_tools/run.py", "--offline"]
