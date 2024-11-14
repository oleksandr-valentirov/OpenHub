import socket
import threading

from argparse import ArgumentParser
from typing import Dict, Union

ConnectionData = Dict[str, Union[socket.socket, Dict[str, any], threading.Lock]]
connections: Dict[str, ConnectionData] = {}


def handle_client(address):
    print(f"Connected to {address}")
    client_socket = connections[address]["socket"]
    mutex = connections[address]["mutex"]

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
        connections.remove(address)


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
            connections[address] = {"sock": client_socket, "data": {}, "mutex": threading.Lock()}
            client_thread = threading.Thread(target=handle_client, args=(address, ))
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
