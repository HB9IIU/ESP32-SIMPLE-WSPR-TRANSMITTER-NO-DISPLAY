import sys
import serial
from PyQt5 import QtWidgets
import pyqtgraph as pg

class SweepAnalyzer(QtWidgets.QWidget):
    def __init__(self, port='COM13', baudrate=115200):
        super().__init__()
        self.setWindowTitle("ESP32 Sweep Analyzer 📡")
        self.resize(900, 600)

        # Initialize plots
        self.plotWidgetCoarse = pg.PlotWidget(title="Coarse Sweep")
        self.plotWidgetFine = pg.PlotWidget(title="Fine Sweep")

        layout = QtWidgets.QVBoxLayout()
        layout.addWidget(self.plotWidgetCoarse)
        layout.addWidget(self.plotWidgetFine)
        self.setLayout(layout)

        # Start serial reading
        self.ser = serial.Serial(port, baudrate, timeout=0.1)
        self.buffer = []
        self.coarseData = []
        self.fineData = []
        self.coarseMaxFreq = 0
        self.fineMaxRawFreq = 0
        self.fineMaxSmoothedFreq = 0

        self.timer = pg.QtCore.QTimer()
        self.timer.timeout.connect(self.read_serial)
        self.timer.start(10)

    def read_serial(self):
        try:
            line = self.ser.readline().decode(errors='ignore').strip()
            if line:
                self.buffer.append(line)
                if line == "SWEEP_DONE":
                    self.parse_buffer()
                    self.plot_results()
                    self.buffer.clear()
        except Exception as e:
            print(f"Serial read error: {e}")

    def parse_buffer(self):
        self.coarseData.clear()
        self.fineData.clear()
        self.coarseMaxFreq = 0
        self.fineMaxRawFreq = 0
        self.fineMaxSmoothedFreq = 0

        for line in self.buffer:
            if line.startswith("COARSE;"):
                _, idx, freq_khz, adc = line.split(";")
                self.coarseData.append((int(freq_khz), int(adc)))
            elif line.startswith("FINE;"):
                _, idx, freq_khz, adc = line.split(";")
                self.fineData.append((int(freq_khz), int(adc)))
            elif line.startswith("COARSE_MAX;"):
                _, adc, freq = line.split(";")
                self.coarseMaxFreq = int(freq) // 1000  # convert to kHz
            elif line.startswith("FINE_MAX_RAW;"):
                _, adc, freq = line.split(";")
                self.fineMaxRawFreq = int(freq) // 1000
            elif line.startswith("FINE_MAX_SMOOTHED;"):
                _, adc, freq = line.split(";")
                self.fineMaxSmoothedFreq = int(freq) // 1000

    def plot_results(self):
        # Plot Coarse
        self.plotWidgetCoarse.clear()
        if self.coarseData:
            x, y = zip(*self.coarseData)
            self.plotWidgetCoarse.plot(x, y, pen='y', symbol='o')
            self.add_vertical_line(self.plotWidgetCoarse, self.coarseMaxFreq, "red", "Max Raw")
            self.add_thin_max_hline(self.plotWidgetCoarse, self.coarseData)

        # Plot Fine
        self.plotWidgetFine.clear()
        if self.fineData:
            x, y = zip(*self.fineData)
            self.plotWidgetFine.plot(x, y, pen='c')
            self.add_vertical_line(self.plotWidgetFine, self.fineMaxRawFreq, "r", "Raw")
            # self.add_vertical_line(self.plotWidgetFine, self.fineMaxSmoothedFreq, "m", "SMA")
            self.add_thin_max_hline(self.plotWidgetFine, self.fineData)


    def add_vertical_line(self, plot, xval, color, label):
        vline = pg.InfiniteLine(pos=xval, angle=90, pen=pg.mkPen(color, width=2))
        plot.addItem(vline)
        text = pg.TextItem(f"{label}: {xval} kHz", anchor=(0,1), color=color)
        text.setPos(xval, plot.viewRange()[1][1])
        plot.addItem(text)

    def add_thin_max_hline(self, plot, data, color='w'):
        if not data:
            return
        _, max_adc = max(data, key=lambda t: t[1])
        hline = pg.InfiniteLine(pos=max_adc, angle=0, pen=pg.mkPen(color, width=1, style=pg.QtCore.Qt.DashLine))
        plot.addItem(hline)



if __name__ == "__main__":
    app = QtWidgets.QApplication(sys.argv)
    window = SweepAnalyzer()
    window.show()
    sys.exit(app.exec_())
