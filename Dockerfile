# ESP32-C3 Development Environment
FROM espressif/idf:release-v5.3

# Install additional tools
RUN apt-get update && apt-get install -y \
    clang-format \
    clang-tidy \
    vim \
    && rm -rf /var/lib/apt/lists/*

COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

# Set up working directory
WORKDIR /project

ENTRYPOINT ["/entrypoint.sh"]
CMD ["/bin/bash"]
