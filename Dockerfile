FROM debian

# Install Utilities(gcc/make)
RUN apt-get update && \
    apt-get -y install gcc mono-mcs && \
    apt-get -y install valgrind &&\
    apt-get -y install make &&\
    rm -rf /var/lib/apt/lists/*

# Setup App
RUN mkdir -p /app
WORKDIR /app
COPY ./app .

RUN make exec
