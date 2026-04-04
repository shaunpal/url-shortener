# Use the official Ubuntu image as the base image
FROM ubuntu:latest

# Set the working directory in the container
WORKDIR /build

# Install necessary dependencies
RUN apt-get update && apt-get install -y \
    g++ \
    libcpprest-dev \
    libboost-all-dev \
    libssl-dev \
    cmake

# Copy the source code into the container

COPY CMakeLists.txt .
COPY src/ src/

# Build the application
RUN cmake . && make

# Expose the port on which the API will listen
EXPOSE 8080

# Command to run the API when the container starts
CMD ["./server"]