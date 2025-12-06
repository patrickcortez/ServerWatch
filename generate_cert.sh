#!/bin/bash
set -e

# Generate a private key
openssl genrsa -out server.key 2048

# Generate a self-signed certificate
openssl req -new -x509 -key server.key -out server.crt -days 365 -subj "/CN=ServerWatch"

echo "Generated server.key and server.crt"
