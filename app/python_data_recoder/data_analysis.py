import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns
from scipy import signal
import numpy as np
import os
from tkinter import filedialog
import tkinter as tk

def select_csv_file():
    root = tk.Tk()
    root.withdraw()  # Hide the main window
    
    # Start in the current directory
    initial_dir = os.path.dirname(os.path.abspath(__file__))
    
    file_path = filedialog.askopenfilename(
        initialdir=initial_dir,
        title="Select CSV file",
        filetypes=(("CSV files", "*.csv"), ("All files", "*.*"))
    )
    
    return file_path if file_path else None

def preprocess_signal(signal_data):
    # Remove DC component (mean)
    signal_ac = signal_data - np.mean(signal_data)
    # Normalize to [-1, 1] range
    signal_norm = signal_ac / np.max(np.abs(signal_ac))
    return signal_norm

def apply_lowpass_filter(data, cutoff_freq, fs):
    """Apply IIR low-pass Butterworth filter"""
    nyquist = fs / 2
    normal_cutoff = cutoff_freq / nyquist
    b, a = signal.butter(4, normal_cutoff, btype='low')
    filtered_data = signal.filtfilt(b, a, data)
    return filtered_data

def apply_highpass_filter(data, cutoff_freq, fs):
    """Apply IIR high-pass Butterworth filter"""
    nyquist = fs / 2
    normal_cutoff = cutoff_freq / nyquist
    b, a = signal.butter(4, normal_cutoff, btype='high')
    filtered_data = signal.filtfilt(b, a, data)
    return filtered_data

def apply_bandpass_filter(data, low_cutoff, high_cutoff, fs):
    """Apply bandpass filter (combination of high-pass and low-pass)"""
    # Apply high-pass first
    highpass_data = apply_highpass_filter(data, low_cutoff, fs)
    # Then apply low-pass
    bandpass_data = apply_lowpass_filter(highpass_data, high_cutoff, fs)
    return bandpass_data

def load_and_plot_data(file_path):
    # Read CSV file
    df = pd.read_csv(file_path)

    # Only take first 500 samples
    df = df[2000:2500]
    
    # Signal processing parameters
    fs = 50  # Sampling frequency (Hz)
    low_cutoff = 0.2  # High-pass cutoff frequency (Hz)
    high_cutoff = 5  # Low-pass cutoff frequency (Hz)
    
    # Process PPG signals and accelerometer data
    ppg_signals = ['R', 'IR', 'G']
    acc_signals = ['X']  # Changed to only include X-axis
    
    # Process accelerometer signal
    for col in ppg_signals + acc_signals:
        # Remove DC component first
        df[f'{col}_dc_removed'] = preprocess_signal(df[col])
        # Apply bandpass filter (0.5-5 Hz)
        df[f'{col}_filtered'] = apply_bandpass_filter(
            df[f'{col}_dc_removed'], 
            low_cutoff, 
            high_cutoff, 
            fs
        )
    
    # Create figure with subplots
    fig = plt.figure(figsize=(10, 8))
    
    # Plot raw PPG signals
    plt.subplot(2, 1, 1)
    for col, color in zip(ppg_signals, ['r', 'y', 'g']):
        plt.plot(df[f'{col}_filtered'], f'{color}-', 
                label=f'{col} (Filtered)', alpha=0.7)
    plt.title(f'PPG Signals (DC Removed + Bandpass {low_cutoff}-{high_cutoff}Hz)')
    plt.ylabel('Intensity')
    plt.legend(loc='upper right', fontsize=8)
    plt.grid(True)
    
    # Plot filtered X-axis accelerometer data
    plt.subplot(2, 1, 2)
    plt.plot(df['X_filtered'], 'r-', 
            label='X-axis (Filtered)', alpha=0.7)
    plt.title(f'X-axis Acceleration (DC Removed + Bandpass {low_cutoff}-{high_cutoff}Hz)')
    plt.xlabel('Sample Number')
    plt.ylabel('Normalized Amplitude')
    plt.legend(loc='upper right', fontsize=8)
    plt.grid(True)
    
    plt.tight_layout()
    plt.show()

def analyze_signals(file_path):
    # Read data
    df = pd.read_csv(file_path)
    
    # Preprocess signals
    for col in ['R', 'IR', 'G', 'X', 'Y', 'Z']:
        df[f'{col}_processed'] = preprocess_signal(df[col].values)
    
    # Calculate basic statistics for processed signals
    stats = df[[col + '_processed' for col in ['R', 'IR', 'G', 'X', 'Y', 'Z']]].describe()
    print("\nProcessed Signal Statistics:")
    print(stats)
    
    # Calculate FFT for processed PPG signals
    fs = 50  # Sampling frequency (Hz)
    f, ppg_psd = signal.welch(df['R_processed'], fs, nperseg=1024)
    
    # Plot FFT
    plt.figure(figsize=(10, 6))
    plt.semilogy(f, ppg_psd)
    plt.title('Power Spectral Density of Processed Red PPG Signal')
    plt.xlabel('Frequency [Hz]')
    plt.ylabel('Power Spectral Density')
    plt.grid(True)
    plt.show()
    
    # Plot correlation heatmap for processed signals
    processed_cols = [col + '_processed' for col in ['R', 'IR', 'G', 'X', 'Y', 'Z']]
    plt.figure(figsize=(10, 8))
    sns.heatmap(df[processed_cols].corr(), 
                annot=True, 
                cmap='coolwarm', 
                center=0,
                fmt='.2f')
    plt.title('Processed Signal Correlation Matrix')
    plt.tight_layout()
    plt.show()

def main():
    # Let user select the CSV file
    file_path = select_csv_file()
    
    if file_path and os.path.exists(file_path):
        print(f"Analyzing data from: {file_path}")
        
        # Plot raw signals
        load_and_plot_data(file_path)
        
        # Perform analysis
        # analyze_signals(file_path)
    else:
        print("No file selected or file does not exist.")

if __name__ == "__main__":
    main()