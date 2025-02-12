import sys
import numpy as np
from PyQt5.QtWidgets import QApplication, QMainWindow, QGridLayout, QWidget, QPushButton, QMessageBox, QVBoxLayout, QFileDialog, QComboBox, QHBoxLayout
from PyQt5.QtSerialPort import QSerialPort
from PyQt5.QtGui import QFont
import pyqtgraph as pg
import csv
import signal
from datetime import datetime  # Add this import at the top of the file

class CircularBuffer:
    def __init__(self, capacity):
        self.capacity = capacity
        self.buffer = np.zeros(capacity)
        self.index = 0
        self.full = False

    def push(self, value):
        self.buffer[self.index] = value
        self.index = (self.index + 1) % self.capacity
        if self.index == 0:
            self.full = True

    def get_data(self):
        if self.full:
            return np.concatenate((self.buffer[self.index:], self.buffer[:self.index]))
        else:
            return self.buffer[:self.index]

class SerialPlotterWindow(QMainWindow):
    def __init__(self):
        super().__init__()

        self.setWindowTitle("Real-Time Py Serial Viewer")
        self.setGeometry(100, 100, 1200, 800)  # Increased window size

        self.layout = QGridLayout()

        self.graph_widgets = []
        self.data_buffers = []
        self.plot_data_items = []
        self.sample_index = 0

        self.buffer_sizes = [1000, 3000, 5000, 7000, 10000]
        self.buffer_capacity = self.buffer_sizes[0]

        self.serial_port = QSerialPort()
        self.serial_port.setPortName("COM8")
        self.serial_port.setBaudRate(115200)
        self.serial_port.readyRead.connect(self.receive_serial_data)

        self.data_records = {}
        self.current_record = {"C": 0, "R": 0, "IR": 0, "G": 0, "X": 0, "Y": 0, "Z": 0}
        self.ppg_updated = False
        self.acc_updated = False
        self.sample_count = 0

    def add_graph(self, name, x_label, y_label, row, col, color):
        # Modified to handle more subplots better
        graph_widget = pg.PlotWidget()
        graph_widget.setBackground("#000000")
        graph_widget.showGrid(True, True)
        graph_widget.setLabel("left", y_label)
        graph_widget.setLabel("bottom", x_label)
        graph_widget.setMouseEnabled(x=True, y=False)
        graph_widget.setClipToView(True)
        
        # Add title to each subplot
        graph_widget.setTitle(name, color=color, size="12pt")
        
        # Set fixed height for consistent sizing
        graph_widget.setMinimumHeight(200)
        graph_widget.setMaximumHeight(400)

        # Use QVBoxLayout for better control
        graph_widget_item = QWidget()
        graph_widget_layout = QVBoxLayout()
        graph_widget_layout.setContentsMargins(5, 5, 5, 5)  # Add margins
        graph_widget_layout.addWidget(graph_widget)
        graph_widget_item.setLayout(graph_widget_layout)
        
        # Add to grid with proper spanning
        self.layout.addWidget(graph_widget_item, row, col)
        
        data_buffer = CircularBuffer(self.buffer_capacity)
        plot_data_item = graph_widget.plot(data_buffer.get_data(), pen=pg.mkPen(color, width=2))
        
        self.data_buffers.append(data_buffer)
        self.plot_data_items.append(plot_data_item)
        self.graph_widgets.append(graph_widget)

    def receive_serial_data(self):
        while self.serial_port.canReadLine():
            try:
                data = self.serial_port.readLine()
                line = data.data().decode("utf-8").strip()
                print(f"Raw line: {line}")  # Debug print
                
                values = line.split(",")
                
                # Check if this is PPG or accelerometer data
                is_ppg_data = False
                is_acc_data = False
                
                for value in values:
                    if ":" in value:
                        sensor_data = value.split(":")
                        sensor_name = sensor_data[0].strip()
                        sensor_value = float(sensor_data[1])
                        
                        # Update current record
                        self.current_record[sensor_name] = sensor_value
                        
                        # Check data type
                        if sensor_name in ["C", "R", "IR", "G"]:
                            is_ppg_data = True
                        elif sensor_name in ["X", "Y", "Z"]:
                            is_acc_data = True
                            
                        # Update plotting buffer
                        sensor_map = {"C": 0, "R": 1, "IR": 2, "G": 3, "X": 4, "Y": 5, "Z": 6}
                        if sensor_name in sensor_map:
                            buffer_index = sensor_map[sensor_name]
                            if buffer_index < len(self.data_buffers):
                                data_buffer = self.data_buffers[buffer_index]
                                data_buffer.push(sensor_value)
                                self.plot_data_items[buffer_index].setData(data_buffer.get_data())
                
                # Update flags
                if is_ppg_data:
                    self.ppg_updated = True
                if is_acc_data:
                    self.acc_updated = True
                
                # Store record only when we have both PPG and ACC updates
                if self.ppg_updated and self.acc_updated:
                    if all(value != 0 for value in self.current_record.values()):
                        self.sample_count += 1
                        self.data_records[self.sample_count] = self.current_record.copy()
                        # Reset flags
                        self.ppg_updated = False
                        self.acc_updated = False
                    
            except (UnicodeDecodeError, IndexError, ValueError) as e:
                print(f"Error parsing line: {e}")

    def export_data(self):
        if len(self.data_records) > 0:
            # Generate default filename with timestamp
            timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
            default_filename = f"sensor_data_{timestamp}.csv"
            
            filename, _ = QFileDialog.getSaveFileName(
                self, 
                "Export Data", 
                default_filename,  # Set default filename
                "CSV Files (*.csv)"
            )
            
            if filename:
                try:
                    with open(filename, "w", newline="") as file:
                        writer = csv.writer(file)
                        # Write header
                        writer.writerow(["C", "R", "IR", "G", "X", "Y", "Z"])
                        # Write data rows
                        for record in self.data_records.values():
                            writer.writerow([
                                record["C"],
                                record["R"],
                                record["IR"],
                                record["G"],
                                record["X"],
                                record["Y"],
                                record["Z"]
                            ])
                    QMessageBox.information(
                        self, "Export Success", f"Data exported to {filename}")
                except Exception as e:
                    print(f"Export error: {e}")
                    QMessageBox.warning(
                        self, "Export Error", "Failed to export data.")
            else:
                QMessageBox.warning(self, "Export Error", "Invalid filename.")
        else:
            QMessageBox.warning(self, "Export Error", "No data to export.")

    def change_buffer_size(self, index):
        self.buffer_capacity = self.buffer_sizes[index]
        self.data_buffers = [CircularBuffer(self.buffer_capacity) for _ in range(len(self.data_buffers))]

    def closeEvent(self, event):
        self.serial_port.close()
        event.accept()

def keyboard_interrupt_handler(signal, frame):
    sys.exit(0)

if __name__ == "__main__":
    app = QApplication(sys.argv)

    signal.signal(signal.SIGINT, keyboard_interrupt_handler)

    app.setStyleSheet("""
        QMainWindow {
            background-color: #050505;
        }
        
        QPushButton {
            background-color: #4CAF50;
            color: white;
            border: none;
            padding: 8px 16px;
            font-size: 14px;
            font-weight: bold;
        }
        
        QPushButton:hover {
            background-color: #45a049;
            cursor: pointer;
        }
        
        QLabel {
            font-size: 12px;
            font-weight: bold;
        }
        
        QComboBox {
            padding: 4px;
            font-size: 12px;
        }
    """)

    plotter_window = SerialPlotterWindow()
    
    # Reorganize subplot layout (2x4 grid)
    plotter_window.add_graph("Count", "Sample", "Value", 0, 0, "w")
    plotter_window.add_graph("Red", "Sample", "Value", 0, 1, "r")
    plotter_window.add_graph("IR", "Sample", "Value", 0, 2, "y")
    plotter_window.add_graph("Green", "Sample", "Value", 0, 3, "g")
    plotter_window.add_graph("Acc X", "Sample", "Value", 1, 0, "r")
    plotter_window.add_graph("Acc Y", "Sample", "Value", 1, 1, "g")
    plotter_window.add_graph("Acc Z", "Sample", "Value", 1, 2, "y")

    # Move controls to bottom
    controls_widget = QWidget()
    controls_layout = QHBoxLayout()
    
    buffer_size_combo = QComboBox()
    buffer_size_combo.addItems([str(size) for size in plotter_window.buffer_sizes])
    buffer_size_combo.setCurrentIndex(0)
    buffer_size_combo.currentIndexChanged.connect(plotter_window.change_buffer_size)
    
    export_button = QPushButton("Export Data")
    export_button.clicked.connect(plotter_window.export_data)
    
    controls_layout.addWidget(buffer_size_combo)
    controls_layout.addWidget(export_button)
    controls_widget.setLayout(controls_layout)
    
    plotter_window.layout.addWidget(controls_widget, 2, 0, 1, 4)  # Span across all columns

    if plotter_window.serial_port.open(QSerialPort.ReadWrite):
        main_widget = QWidget()
        main_widget.setLayout(plotter_window.layout)
        plotter_window.setCentralWidget(main_widget)
        plotter_window.show()
        sys.exit(app.exec_())
    else:
        print("Failed to open serial port.")
        sys.exit(1)