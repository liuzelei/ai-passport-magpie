import CoreBluetooth
import Darwin
import Foundation

private let serviceID = CBUUID(string: "D1749A01-65B4-46F7-A6BA-DA9D2AB61750")
private let writeID = CBUUID(string: "D1749A02-65B4-46F7-A6BA-DA9D2AB61750")
private let notifyID = CBUUID(string: "D1749A03-65B4-46F7-A6BA-DA9D2AB61750")
private let deviceName = "FoloToy AI Passport"

private func emit(_ line: String, to file: FileHandle = .standardOutput) {
    file.write(Data((line + "\n").utf8))
}

private struct Command {
    var scanOnly = false
    var device: UUID?

    init?(_ arguments: [String]) {
        switch arguments {
        case []:
            break
        case ["--scan"]:
            scanOnly = true
        default:
            guard arguments.count == 2, arguments[0] == "--device",
                  let id = UUID(uuidString: arguments[1]) else { return nil }
            device = id
        }
    }
}

private struct Lines {
    private var bytes: [UInt8] = []
    private var discarding = false

    mutating func receive(_ data: [UInt8]) -> [String] {
        var lines: [String] = []
        for byte in data {
            if byte == 10 {
                if !discarding {
                    if bytes.last == 13 { bytes.removeLast() }
                    if let line = String(bytes: bytes, encoding: .ascii), !line.isEmpty {
                        lines.append(line)
                    }
                }
                bytes.removeAll(keepingCapacity: true)
                discarding = false
            } else if !discarding {
                if byte == 0 || bytes.count == 159 {
                    discarding = true
                } else {
                    bytes.append(byte)
                }
            }
        }
        return lines
    }
}

private struct Transfer {
    let reply: String
    let chunks: [Data]
    var offset = 0
    var notified = false

    init(line: String, reply: String) {
        self.reply = reply
        // 20 bytes fits one write at the default ATT MTU of 23.
        let frame = Array(("\n" + line + "\n").utf8)
        chunks = stride(from: 0, to: frame.count, by: 20).map { start in
            Data(frame[start..<min(start + 20, frame.count)])
        }
    }
}

private final class Transport: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    private enum Phase { case starting, scanning, connecting, probing, ready }

    private let command: Command
    private var phase = Phase.starting
    private var central: CBCentralManager!
    private var candidates: [UUID: (peripheral: CBPeripheral, name: String)] = [:]
    private var peer: CBPeripheral?
    private var writer: CBCharacteristic?
    private var transfer: Transfer?
    private var transferTimeout: DispatchWorkItem?
    private var stdinSource: DispatchSourceRead?
    private var input = Lines()
    private var notifications = Lines()
    private var announcedPairing = false

    init(command: Command) {
        self.command = command
    }

    func start() {
        if !command.scanOnly {
            let source = DispatchSource.makeReadSource(fileDescriptor: STDIN_FILENO, queue: .main)
            source.setEventHandler { [weak self] in self?.consumeInput() }
            source.resume()
            stdinSource = source
        }
        central = CBCentralManager(delegate: self, queue: .main,
                                   options: [CBCentralManagerOptionShowPowerAlertKey: false])
        DispatchQueue.main.asyncAfter(deadline: .now() + 85) { [weak self] in
            guard let self, phase != .ready else { return }
            fail("Bluetooth connection timed out. Keep the Passport on and complete pairing on the Mac.")
        }
    }

    private func fail(_ message: String, status: Int32 = 1) -> Never {
        emit(message, to: .standardError)
        exit(status)
    }

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            guard phase == .starting else { return }
            phase = .scanning
            central.scanForPeripherals(withServices: [serviceID])
            DispatchQueue.main.asyncAfter(deadline: .now() + 3) { [weak self] in self?.finishScan() }
        case .poweredOff:
            fail("Bluetooth is off. Turn it on in System Settings.")
        case .unauthorized:
            fail("Allow AI Passport BLE to use Bluetooth in System Settings → Privacy & Security → Bluetooth.", status: 3)
        case .unsupported:
            fail("This Mac does not support Bluetooth Low Energy.", status: 3)
        case .resetting, .unknown:
            if phase == .ready { fail("Bluetooth became unavailable. Reconnecting.") }
        @unknown default:
            fail("Bluetooth is unavailable on this Mac.")
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi: NSNumber) {
        guard phase == .scanning,
              (advertisementData[CBAdvertisementDataIsConnectable] as? Bool) != false else { return }
        let name = advertisementData[CBAdvertisementDataLocalNameKey] as? String
            ?? candidates[peripheral.identifier]?.name ?? peripheral.name ?? "(name unavailable)"
        candidates[peripheral.identifier] = (peripheral, name)
    }

    private func finishScan() {
        guard phase == .scanning else { return }
        central.stopScan()
        let identifiers = candidates.keys.sorted { $0.uuidString < $1.uuidString }
        let devices = identifiers.compactMap { id in
            candidates[id].map { candidate in
                let name = candidate.name.components(separatedBy: .controlCharacters).joined(separator: " ")
                return "\(id.uuidString)\t\(name)"
            }
        }
        if command.scanOnly {
            if identifiers.isEmpty {
                emit("No \(deviceName) found. Turn on a Passport running the token usage firmware.", to: .standardError)
            }
            for device in devices { emit(device) }
            exit(0)
        }
        if command.device == nil && identifiers.count > 1 {
            for device in devices { emit("  \(device)", to: .standardError) }
            fail("Multiple Passport devices found. Specify one with --device UUID.", status: 2)
        }
        guard let id = command.device ?? identifiers.first, let candidate = candidates[id]?.peripheral else {
            fail("No matching \(deviceName) found. Turn on the device and check its BLE firmware.")
        }
        phase = .connecting
        peer = candidate
        candidate.delegate = self
        central.connect(candidate)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        guard peripheral === peer else { return }
        peripheral.discoverServices([serviceID])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        if peripheral === peer { fail("Could not connect to the \(deviceName) over Bluetooth.") }
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        if peripheral === peer { fail("\(deviceName) Bluetooth connection ended.") }
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard peripheral === peer else { return }
        guard error == nil, let service = peripheral.services?.first(where: { $0.uuid == serviceID }) else {
            fail("The Passport token usage service is unavailable.")
        }
        peripheral.discoverCharacteristics([writeID, notifyID], for: service)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard peripheral === peer else { return }
        guard error == nil,
              let request = service.characteristics?.first(where: { $0.uuid == writeID }),
              request.properties.contains(.write),
              let reply = service.characteristics?.first(where: { $0.uuid == notifyID }),
              reply.properties.contains(.notify) else {
            fail("The Passport has an incompatible Bluetooth usage service.")
        }
        writer = request
        peripheral.setNotifyValue(true, for: reply)
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic,
                    error: Error?) {
        guard peripheral === peer, characteristic.uuid == notifyID else { return }
        if let error, phase == .connecting, needsPairing(error) {
            retryPairing { peripheral.setNotifyValue(true, for: characteristic) }
            return
        }
        guard error == nil, characteristic.isNotifying else {
            fail("Could not subscribe to Passport Bluetooth notifications.")
        }
        if phase == .connecting {
            phase = .probing
            begin("MPU1,PING", reply: "MPU1,PONG")
        }
    }

    private func needsPairing(_ error: Error) -> Bool {
        let issue = error as NSError
        return issue.domain == CBATTErrorDomain && [
            CBATTError.Code.insufficientAuthentication.rawValue,
            CBATTError.Code.insufficientEncryption.rawValue,
            CBATTError.Code.insufficientEncryptionKeySize.rawValue,
        ].contains(issue.code)
    }

    private func retryPairing(_ action: @escaping () -> Void) {
        if !announcedPairing {
            announcedPairing = true
            emit("If macOS requests pairing, enter the passkey shown on the Passport.", to: .standardError)
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 2, execute: action)
    }

    private func begin(_ line: String, reply: String) {
        transfer = Transfer(line: line, reply: reply)
        if phase == .ready {
            let timeout = DispatchWorkItem { [weak self] in self?.fail("The Passport did not acknowledge the update.") }
            transferTimeout = timeout
            DispatchQueue.main.asyncAfter(deadline: .now() + 7, execute: timeout)
        }
        writeNext()
    }

    private func writeNext() {
        guard let peer, let writer, let current = transfer, current.offset < current.chunks.count else { return }
        peer.writeValue(current.chunks[current.offset], for: writer, type: .withResponse)
    }

    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        guard peripheral === peer, characteristic.uuid == writeID, transfer != nil else { return }
        if let error {
            if phase == .probing && needsPairing(error) {
                retryPairing { [weak self] in self?.writeNext() }
            } else {
                fail("The Passport rejected the Bluetooth write.")
            }
            return
        }
        transfer?.offset += 1
        writeNext()
        completeIfReady()
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard peripheral === peer, characteristic.uuid == notifyID, let current = transfer else { return }
        guard error == nil, let bytes = characteristic.value else {
            fail("Could not read a Passport Bluetooth notification.")
        }
        if notifications.receive(Array(bytes)).contains(current.reply) {
            transfer?.notified = true
            completeIfReady()
        }
    }

    private func completeIfReady() {
        guard let current = transfer, current.notified, current.offset == current.chunks.count else { return }
        transfer = nil
        transferTimeout?.cancel()
        transferTimeout = nil
        if phase == .probing {
            phase = .ready
            emit("READY")
        } else {
            emit("ACK")
        }
    }

    private func consumeInput() {
        var buffer = [UInt8](repeating: 0, count: 256)
        let size = Darwin.read(STDIN_FILENO, &buffer, buffer.count)
        if size == 0 { exit(0) }
        if size < 0 {
            if errno == EINTR || errno == EAGAIN { return }
            fail("Could not read from the Magpie bridge.")
        }
        for line in input.receive(Array(buffer.prefix(size))) {
            guard phase == .ready, transfer == nil else { fail("The Passport is not ready for another update.", status: 2) }
            guard line == "MPU1,ERR" || line.hasPrefix("MPU1,OK,") else { fail("Invalid Magpie bridge frame.", status: 2) }
            begin(line, reply: "MPU1,ACK")
        }
    }
}

private let usage = "Usage: ai-passport-ble [--scan | --device UUID]"
private let arguments = Array(CommandLine.arguments.dropFirst())
if arguments == ["--help"] || arguments == ["-h"] {
    emit(usage)
    exit(0)
}
guard let command = Command(arguments) else {
    emit(usage, to: .standardError)
    exit(2)
}
private let transport = Transport(command: command)
transport.start()
dispatchMain()
