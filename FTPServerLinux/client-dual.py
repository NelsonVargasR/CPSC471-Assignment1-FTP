from socket import *

def setup_ephemeral_data_channel():
    """Creates a temporary socket bound to an OS-assigned ephemeral port."""
    data_socket = socket(AF_INET, SOCK_STREAM)
    data_socket.bind(('', 0))  # Port 0 tells OS to pick any free port
    data_socket.listen(1)
    ephemeral_port = data_socket.getsockname()[1]
    return data_socket, ephemeral_port

def run_ftp_client(server_name, server_port):
    control_socket = socket(AF_INET, SOCK_STREAM)
    control_socket.connect((server_name, server_port))
    print(f"Connected to FTP server at {server_name}:{server_port} via control channel.")

    try:
        while True:
            user_input = input("ftp> ").strip()
            if not user_input:
                continue

            parts = user_input.split()
            cmd = parts[0].lower()

            if cmd == "quit":
                control_socket.send(user_input.encode())
                break

            if cmd in ["get", "put", "ls"]:
                data_listener, eph_port = setup_ephemeral_data_channel()
                control_message = f"{user_input} {eph_port}"
                control_socket.send(control_message.encode())

                print(f"Waiting for data channel connection on ephemeral port {eph_port}...")
                data_conn, server_addr = data_listener.accept()

                if cmd == "get" or cmd == "ls":
                    received_data = b""
                    while True:
                        chunk = data_conn.recv(4096)
                        if not chunk:
                            break
                        received_data += chunk
                    print(f"SUCCESS: Transferred {len(received_data)} bytes via data channel.")

                elif cmd == "put":
                    filename = parts[1]
                    with open(filename, "rb") as f:
                        file_bytes = f.read()
                    bytes_sent = 0
                    while bytes_sent != len(file_bytes):
                        sent = data_conn.send(file_bytes[bytes_sent:])
                        bytes_sent += sent
                    print(f"SUCCESS: Uploaded {bytes_sent} bytes.")

                data_conn.close()
                data_listener.close()

            else:
                control_socket.send(user_input.encode())

            response = control_socket.recv(1024).decode()
            print(f"Server Response: {response}")

    finally:
        control_socket.close()
        print("Control channel closed. Exiting.")

if __name__ == "__main__":
    run_ftp_client("localhost", 12000)

from socket import *
import os

def run_ftp_server(server_port):
    server_socket = socket(AF_INET, SOCK_STREAM)
    server_socket.bind(('', server_port))
    server_socket.listen(5)
    print(f"FTP Server listening for control connections on port {server_port}...")

    while True:
        control_conn, client_addr = server_socket.accept()
        print(f"Accepted control connection from {client_addr}")

        try:
            while True:
                message = control_conn.recv(1024).decode()
                if not message:
                    break

                parts = message.split()
                cmd = parts[0].lower()

                if cmd == "quit":
                    control_conn.send("SUCCESS: Disconnected".encode())
                    break

                if cmd in ["get", "put", "ls"]:
                    filename = parts[1] if len(parts) > 1 else None
                    client_eph_port = int(parts[-1])

                    print(f"Connecting back to client data channel at {client_addr[0]}:{client_eph_port}...")
                    data_conn = socket(AF_INET, SOCK_STREAM)
                    data_conn.connect((client_addr[0], client_eph_port))

                    if cmd == "get":
                        if filename and os.path.exists(filename):
                            with open(filename, "rb") as f:
                                file_bytes = f.read()
                            bytes_sent = 0
                            while bytes_sent != len(file_bytes):
                                sent = data_conn.send(file_bytes[bytes_sent:])
                                bytes_sent += sent
                            control_conn.send("SUCCESS".encode())
                        else:
                            control_conn.send("FAILURE: File not found".encode())

                    elif cmd == "put":
                        received_data = b""
                        while True:
                            chunk = data_conn.recv(4096)
                            if not chunk:
                                break
                            received_data += chunk
                        if filename:
                            with open(filename, "wb") as f:
                                f.write(received_data)
                            control_conn.send("SUCCESS".encode())
                        else:
                            control_conn.send("FAILURE: Invalid filename".encode())

                    data_conn.close()
                else:
                    control_conn.send("FAILURE: Unknown command".encode())

        except Exception as e:
            print(f"Error handling client session: {e}")
        finally:
            control_conn.close()
            print(f"Control connection closed for {client_addr}")

if __name__ == "__main__":
    run_ftp_server(12000)
