# Use an official Ubuntu image as the base
FROM ubuntu:latest

# Update apt and install essential development tools
RUN apt-get update && apt-get install -y \
    build-essential \
    gcc \
    gdb \
    make \
    curl \
    wget \
    vim \
    iputils-ping \
    # pthreads library and header
    libpthread-stubs0-dev


# Set the working directory inside the container
WORKDIR /app

# Copy the source code from the host to the container
COPY . .

# Compile the program using make
RUN make all

# Expose the port that your server listens on (change this to your actual port)
EXPOSE 8080

# Command to run when the container starts (change this to your actual executable and port)
CMD ["./proxy", "8080"]