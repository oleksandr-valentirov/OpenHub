<h1>OpenHub project</h1>
<p>
It is a personal open-source project of a hub for wireless sensors.<br>
The reason I want to develop it - I don't want to trust my data to companies with closed-source software<br>
and my data security to their centralized servers.
</p>

<h2>LWIP</h2>
<p>
LWIP configured as discribed <a href="https://community.st.com/t5/stm32-mcus/how-to-create-a-project-for-stm32h7-with-ethernet-and-lwip-stack/ta-p/49308">here</a>.</p>
<p>
LWIP malloc problem solved by replacing malloc and free functions in CM7/Core/Src/newlib_stubs.c<br>
as described <a href="https://community.st.com/t5/stm32-mcus-embedded-software/lwip-rand-uses-newlib-rand-and-fails/m-p/720026/highlight/true#M51347">here</a>.
</p>
<p>Proudly developed in Ukraine :ukraine:</p>