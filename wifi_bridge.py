#!/usr/bin/env python3
"""
WiFi Bridge for STM32 Dynamic Overclock Miner
- Connects to Duino-Coin server
- Sends jobs to STM32 over Serial
- Submits found shares automatically
"""

import serial
import serial.tools.list_ports
import socket
import time
import sys
import argparse

# ==================== CONFIGURATION ====================
DUCO_USER = "YOUR_USERNAME_HERE"  # CHANGE THIS
MINING_KEY = ""                    # Leave empty unless you have one
RIG_NAME = "STM32-Overclock"
BAUDRATE = 115200
# =======================================================

SERVER = "server.duinocoin.com"
PORT = 2811
VERSION = "STM32_Bridge/v2.0"

def find_stm32():
    """Auto-detect connected STM32 via USB serial"""
    ports = serial.tools.list_ports.comports()
    for port in ports:
        desc = port.description.lower()
        if "stm" in desc or "maple" in desc or "serial" in desc or "usb" in desc:
            return port.device
    return None

def connect_serial():
    """Connect to STM32 over Serial"""
    port = find_stm32()
    if not port:
        print("❌ No STM32 found!")
        print("   Check USB connection and upload the code first.")
        sys.exit(1)
    
    print(f"✅ Found STM32 on {port}")
    ser = serial.Serial(port, BAUDRATE, timeout=10)
    time.sleep(2)
    
    # Test connection
    ser.write(b"PING\n")
    start = time.time()
    while time.time() - start < 5:
        if ser.in_waiting:
            response = ser.readline().decode().strip()
            if response == "PONG":
                print("✅ STM32 ready!")
                return ser
            break
    
    print("❌ STM32 not responding!")
    print("   Make sure the code is uploaded and serial monitor is closed.")
    sys.exit(1)

def recv_response(sock, timeout=10):
    """Read full response from DUCO server"""
    sock.settimeout(timeout)
    data = b""
    start = time.time()
    
    while time.time() - start < timeout:
        try:
            chunk = sock.recv(1)
            if chunk:
                data += chunk
                if data.endswith(b'\n'):
                    break
            else:
                time.sleep(0.01)
        except socket.timeout:
            break
    
    return data.decode().strip()

def calculate_timeout(difficulty):
    """Estimate mining time based on difficulty"""
    # STM32 at 96MHz can do ~15-20k H/s
    estimated_seconds = (difficulty * 100) / 15000
    return max(30, min(estimated_seconds, 90))

def main():
    print("=" * 50)
    print("  🚀 STM32 Dynamic Overclock Miner")
    print("=" * 50)
    print(f"👤 User: {DUCO_USER}")
    print(f"🖥️ Rig: {RIG_NAME}")
    
    if DUCO_USER == "YOUR_USERNAME_HERE":
        print("❌ ERROR: Change DUCO_USER to your username!")
        sys.exit(1)
    
    # Connect to STM32
    stm32 = connect_serial()
    
    # Connect to DUCO server
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        sock.connect((SERVER, PORT))
        print(f"✅ Connected to {SERVER}:{PORT}")
    except Exception as e:
        print(f"❌ Cannot connect to server: {e}")
        sys.exit(1)
    
    accepted = 0
    rejected = 0
    total_jobs = 0
    
    print("\n⛏️ Mining started! Press Ctrl+C to stop\n")
    
    try:
        while True:
            # Get job from server
            job_request = f"JOB,{DUCO_USER},{MINING_KEY}\n"
            sock.send(job_request.encode())
            
            job_data = recv_response(sock, 10)
            if not job_data:
                print("❌ No job received, retrying...")
                time.sleep(5)
                continue
            
            parts = job_data.split(',')
            if len(parts) >= 3:
                last_hash = parts[0]
                expected = parts[1]
                difficulty = parts[2]
                total_jobs += 1
                
                print(f"📡 Job #{total_jobs}: diff={difficulty}, target={expected[:16]}...")
                
                # Send job to STM32
                stm32_cmd = f"JOB,{last_hash},{expected},{difficulty}\n"
                stm32.write(stm32_cmd.encode())
                
                # Wait for JOB_READY from STM32
                ready = stm32.readline().decode().strip()
                if ready != "JOB_READY":
                    print(f"⚠️ STM32 not ready: {ready}")
                    continue
                
                print("   🔨 Mining...")
                
                # Wait for result
                result = ""
                timeout = calculate_timeout(int(difficulty))
                start = time.time()
                
                while time.time() - start < timeout:
                    if stm32.in_waiting:
                        result = stm32.readline().decode().strip()
                        break
                    time.sleep(0.1)
                
                if result.startswith("FOUND"):
                    nonce = result.split(',')[1]
                    
                    # Submit share
                    share_request = f"{nonce},0,{VERSION},{RIG_NAME}\n"
                    sock.send(share_request.encode())
                    
                    response = recv_response(sock, 10)
                    
                    if response.startswith("GOOD"):
                        accepted += 1
                        print(f"   ✅ ACCEPTED! (A:{accepted} R:{rejected})")
                    else:
                        rejected += 1
                        print(f"   ❌ REJECTED: {response}")
                        
                elif result == "NONCE_NOT_FOUND":
                    print("   ⏳ No nonce found, getting new job...")
                else:
                    print(f"   ⚠️ Unknown: {result}")
                    
            else:
                print(f"⚠️ Malformed job: {job_data}")
                
    except KeyboardInterrupt:
        print("\n" + "=" * 50)
        print(f"🛑 Mining stopped")
        print(f"📊 Final stats - Accepted: {accepted}, Rejected: {rejected}")
        print(f"📡 Total jobs processed: {total_jobs}")
        print("=" * 50)
    except Exception as e:
        print(f"❌ Error: {e}")
    finally:
        stm32.close()
        sock.close()

if __name__ == "__main__":
    main()