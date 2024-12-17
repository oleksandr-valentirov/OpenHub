import shutil
import os

# Вкажіть шлях до вашого кастомного лінкер-файлу
custom_linker_file_m7 = "CM7/custom_m7_flash.ld"
custom_linker_file_m4 = "CM4/custom_m4_flash.ld"

# Вкажіть шлях до директорії, куди CubeMX генерує код
output_directory_m7 = "CM7/"
output_directory_m4 = "CM4/"

# Вкажіть ім'я лінкер-файлу, який потрібно перезаписати
default_linker_file_m7 = "stm32h755xx_flash_CM7.ld"  # змініть на реальне ім'я
default_linker_file_m4 = "stm32h755xx_flash_CM4.ld"  # змініть на реальне ім'я

# Формуємо повний шлях до файлів
destination_m7 = os.path.join(output_directory_m7, default_linker_file_m7)
destination_m4 = os.path.join(output_directory_m4, default_linker_file_m4)

# Копіюємо кастомний лінкер-файл в директорію проекту
try:
    shutil.copy(custom_linker_file_m7, destination_m7)
    shutil.copy(custom_linker_file_m4, destination_m4)
except Exception as e:
    print(f"Error: {e}")
