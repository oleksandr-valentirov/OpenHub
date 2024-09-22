import shutil
import os

# Вкажіть шлях до вашого кастомного лінкер-файлу
custom_linker_file = "CM7/custom_m7_flash.ld"

# Вкажіть шлях до директорії, куди CubeMX генерує код
output_directory = "CM7/"

# Вкажіть ім'я лінкер-файлу, який потрібно перезаписати
default_linker_file = "stm32h755xx_flash_CM7.ld"  # змініть на реальне ім'я

# Формуємо повний шлях до файлів
destination = os.path.join(output_directory, default_linker_file)

# Копіюємо кастомний лінкер-файл в директорію проекту
try:
    shutil.copy(custom_linker_file, destination)
    print(f"Linker file copied to {destination}")
except Exception as e:
    print(f"Error: {e}")
