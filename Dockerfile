FROM almalinux:9.3 AS build-boost
RUN dnf update -y && dnf install -y git g++ make cmake wget openssl-devel

# Install Boost
# https://www.boost.org/doc/libs/1_80_0/more/getting_started/unix-variants.html
RUN cd /tmp && \    
    wget https://boostorg.jfrog.io/artifactory/main/release/1.81.0/source/boost_1_81_0.tar.gz && \ 
    tar xfz boost_1_81_0.tar.gz && \
    cd boost_1_81_0 && \
    ./bootstrap.sh --prefix=/usr/local && \
    ./b2 install && \
    rm -rf /tmp/*


FROM build-boost AS build-proxy
WORKDIR /build
COPY CMakeLists.txt /build/
COPY src /build/src
RUN cmake .
RUN make -j$(nproc)

# Build of the Web interface for Socks5Balancer
FROM node:18 AS build-webui
RUN apt update && apt install git

WORKDIR /html

# Use the latest version on the git repository
RUN git clone https://github.com/retinio/Socks5BalancerHtml.git .

# Was supposed to retrieve the submodule from the repository but doesn't work with github fork
#COPY html /html

RUN yarn
RUN yarn build

# Runtime image
FROM build-boost AS runtime
WORKDIR /app

COPY --from=build-proxy /build/Socks5BalancerAsio /app/Socks5BalancerAsio
COPY --from=build-webui /html /app/html

CMD ./Socks5BalancerAsio

