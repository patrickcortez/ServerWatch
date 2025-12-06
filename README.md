# Server Watch

---

This is the server component for **ServerWatch**, a self-hosted file sharing solution. It handles secure file transfer and directory monitoring operations.

## Prerequisites

- GCC
- Make
- OpenSSL development libraries (`libssl-dev` or `openssl-devel`)

## Installation & Setup

1. **Generate SSL Certificates**
   First, you need to generate a self-signed certificate and private key for secure communication.
   ```bash
   make cert
   ```
   This will create `server.key` and `server.crt`.

2. **Compile the Server**
   ```bash
   make
   ```

## Usage

To start the server, provide the directory to watch and a password for client authentication.

```bash
# Syntax: ./file_server <directory> <password>
make run <path-to-folder> <password>
```

Example:
```bash
make run ./files mysecretpassword
```

## Created by

- **Cortez**: A 3rd Year Computer Engineering Student