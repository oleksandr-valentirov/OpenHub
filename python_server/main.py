import socket
import threading

from argparse import ArgumentParser

connections = []


def handle_client(client_socket, address):
    print(f"Connected to {address}")
    
    try:
        while True:
            message = client_socket.recv(1024)
            if not message:
                break
            print(f"{address}: {message.decode('utf-8')}")
            client_socket.sendall(b"OK")
    except Exception as e:
        print(f"Error {address}: {e}")
    finally:
        client_socket.close()
        print(f"Connection {address} closed")
        connections.remove(client_socket)


def start_server(host, port, max_connections):
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_socket.bind((host, port))
    server_socket.listen(max_connections)
    print(f"Server started at {host}:{port}, waiting for connections...")

    while True:
        if len(connections) >= max_connections:
            print("Max connections number reached.")
            continue
        
        client_socket, address = server_socket.accept()

        if len(connections) < max_connections:
            connections.append(client_socket)
            client_thread = threading.Thread(target=handle_client, args=(client_socket, address))
            client_thread.start()
        else:
            print("Max connections number reached. Disconnecting.")
            client_socket.close()


if __name__ == "__main__":
    args_parser = ArgumentParser()
    args_parser.add_argument("-a", "address", type=str, required=False, help="IP address of the server", default="127.0.0.1")
    args_parser.add_argument("-p", "port", type=int, required=False, help="Port of the server", default=12345)
    args_parser.add_argument("-c", "connections", type=int, required=False, help="Max number of connections", default=5)
    args = args_parser.parse_args()

    start_server(args.address, args.port, args.connections)
