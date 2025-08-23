
    const apiUrl = 'https://db1.wspr.live:443';
    WSPR_tx_Callsign="HB9IIU";   // XXXXXX
    WSPR_tx_Latitude  = 46.4668752; // Your station latitude
    WSPR_tx_Longitude = 6.8617024;  // Your station longitude
    cesiumAccessToken = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJqdGkiOiIzZjMzZDY3OS0yNjliLTRiMTAtOTgzMS1jZDgwOTUxMTk3YzEiLCJpZCI6MjI5ODU5LCJpYXQiOjE3MjE3Mzg4NTN9.vYEZOFmLFX3fgLX6f0EiLXjvVIHL9HHxac97c0JruZQ";



    const cleanedDataForMaps = [];

    document.addEventListener('DOMContentLoaded', function () {

    const limit = 10000000000; // to set lower when debugging to shorten SQL request returned data

    document.getElementById('txSignLabel').textContent = WSPR_tx_Callsign;

    const bandMapping = {
    '-1': 'LF',
    '0': 'MF',
    '1': '160m (1.8 MHz)',
    '3': '80m (3.5 MHz)',
    '5': '60m (5 MHz)',
    '7': '40m (7 MHz)',
    '10': '30m (10 MHz)',
    '14': '20m (14 MHz)',
    '18': '17m (18 MHz)',
    '21': '15m (21 MHz)',
    '24': '12m (24 MHz)',
    '28': '10m (28 MHz)',
    '50': '6m (50 MHz)',
    '70': '4m (70 MHz)',
    '144': '2m (144 MHz)',
    '432': '70cm (432 MHz)',
    '1296': '23cm (1296 MHz)'
};
    const bandColorMapping = {
    '-1': '#800000',
    '0': '#FF4500',
    '1': '#FFD700',
    '3': '#32CD32',
    '5': '#008000',
    '7': '#0000FF',
    '10': '#4B0082',
    '14': '#EE82EE',
    '18': '#8A2BE2',
    '21': '#FF1493',
    '24': '#FF69B4',
    '28': '#CD5C5C',
    '50': '#4682B4',
    '70': '#00FFFF',
    '144': '#2E8B57',
    '432': '#D2691E',
    '1296': '#FF6347'
};
    const headerMapping = {
    'date': 'Date',
    'time': 'Time',
    'band': 'Band',
    'rx_sign': 'Callsign',
    'rx_loc': 'Locator',
    'distance': 'Distance [km]',
    'frequency': 'Frequency [Hz]',
    'snr': 'SNR [dB]',
    'drift': 'Drift [Hz]'
};


    let selectedBands = Object.keys(bandMapping).map(Number); // Initially select all bands

    const slider = document.getElementById('time-slider');
    const spinner = document.getElementById('loadingSpinner');
    const searchBox = document.getElementById('searchBox');
    const refreshButton = document.getElementById('refreshButton');

    const map = L.map('map', {
    attributionControl: false,
    zoomControl: false
}).setView([WSPR_tx_Latitude, WSPR_tx_Longitude], 2);
    const mapLayerGroup = L.featureGroup().addTo(map);
    L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png').addTo(map);

    const slotDurationElement = document.getElementById('slotDuration');
    const totalSpotsElement = document.getElementById('totalSpots'); // Get the totalSpots element
    const uniqueCallsignsElement = document.getElementById('uniqueCallsigns'); // Get the uniqueCallsigns element
    const maxDistanceElement = document.getElementById('maxDistance'); // Get the maxDistance element

    noUiSlider.create(slider, {
    start: [-120, 0],
    connect: true,
    range: {
    'min': -1440,
    'max': 0
},
    tooltips: [wNumb({decimals: 0}), wNumb({decimals: 0})],
    format: wNumb({decimals: 0, thousand: ''})
});


    slider.noUiSlider.on('update', function (values, handle) {
    const startMinutes = parseInt(values[0]);
    const endMinutes = parseInt(values[1]);
    updateSlotDuration(startMinutes, endMinutes);
    const lowerTooltip = slider.querySelector('.noUi-handle-lower .noUi-tooltip');
    const upperTooltip = slider.querySelector('.noUi-handle-upper .noUi-tooltip');
    if (lowerTooltip) {
    lowerTooltip.innerHTML = formatSliderValueToTime(startMinutes);
}
    if (upperTooltip) {
    upperTooltip.innerHTML = formatSliderValueToTime(endMinutes);
}
});

    slider.noUiSlider.on('set', function (values, handle) {
    const startMinutes = parseInt(values[0]);
    const endMinutes = parseInt(values[1]);
    updateSlotDuration(startMinutes, endMinutes);
    fetchData(startMinutes, endMinutes);
});

    searchBox.addEventListener('input', function () {
    filterTable(searchBox.value.toUpperCase());
});

    refreshButton.addEventListener('click', function () {
    const values = slider.noUiSlider.get();
    const startMinutes = parseInt(values[0]);
    const endMinutes = parseInt(values[1]);
    fetchData(startMinutes, endMinutes);
});

    function fetchData(startMinutes, endMinutes) {
    mapLayerGroup.clearLayers();
    const query = `
                SELECT * FROM wspr.rx
                WHERE time > subtractMinutes(now(), ${-startMinutes})
                AND time <= subtractMinutes(now(), ${-endMinutes})
                AND tx_sign = '${WSPR_tx_Callsign}'
                ORDER BY time DESC
                LIMIT ${limit} FORMAT JSON
            `;
    const url = `${apiUrl}?query=${encodeURIComponent(query)}`;
    //console.log('Executing query:', query);
    spinner.style.display = 'block';
    fetch(url)
    .then(response => {
    if (!response.ok) {
    throw new Error('Network response was not ok');
}
    return response.json();
})
    .then(jsonData => {
    //console.log('Fetched data:', jsonData);
    const data = jsonData.data;
    if (data.length === 0) {
    renderErrorMessage('No data available');
    return;
}
    // here we remove duplicate rx_signs for faster plotting
    const seen = new Set();
    data.forEach(row => {
    const identifier = `${row.rx_lat}-${row.rx_lon}-${row.band}-${row.rx_sign}`; // Create a unique key based on relevant fields

    // If the identifier hasn't been seen yet, add it to the cleanedData array and the seen set
    if (!seen.has(identifier)) {
    cleanedDataForMaps.push(row);
    seen.add(identifier);
}
});

    renderFilteredTable(data);
    updateLeafletMap(cleanedDataForMaps.filter(row => selectedBands.includes(row.band)));

    updateCesiumMap(cleanedDataForMaps.filter(row => selectedBands.includes(row.band)));

    updateLegend(data);
    updateTotalSpots(data); // Update the total spots
    updateUniqueCallsigns(data); // Update the unique callsigns
    updateMaxDistance(data); // Update the max distance
})
    .catch(error => {
    console.error('Error fetching data:', error);
    renderErrorMessage(`Error fetching data: ${error.message}`);
})
    .finally(() => {
    spinner.style.display = 'none';
});
}

    function renderFilteredTable(data) {
    const filteredTableHead = document.getElementById('filtered-table-head');
    const filteredTableBody = document.getElementById('filtered-wspr-data');
    filteredTableHead.innerHTML = '';
    filteredTableBody.innerHTML = '';

    const headers = ['date', 'time', 'band', 'rx_sign', 'rx_loc', 'distance', 'frequency', 'snr', 'drift'];
    const tr = document.createElement('tr');
    headers.forEach(header => {
    const th = document.createElement('th');
    th.textContent = headerMapping[header] || header;
    if (header === 'time' || header === 'date') {
    th.classList.add('time-column');
}
    tr.appendChild(th);
});
    filteredTableHead.appendChild(tr);

    const numberFormatter = new Intl.NumberFormat();

    data.forEach(row => {
    const tr = document.createElement('tr');
    headers.forEach(header => {
    const td = document.createElement('td');
    if (header === 'date') {
    const utcDate = new Date(row['time'] + 'Z');
    td.textContent = `${utcDate.getDate().toString().padStart(2, '0')}:${(utcDate.getMonth() + 1).toString().padStart(2, '0')}:${utcDate.getFullYear()}`;
} else if (header === 'time') {
    const utcDate = new Date(row['time'] + 'Z');
    const localTime = utcDate.toLocaleTimeString([], {
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit'
});
    td.textContent = localTime;
    td.classList.add('time-column');
} else if (header === 'band') {
    td.textContent = bandMapping[row[header]] || row[header];
} else if (header === 'distance') {
    td.textContent = numberFormatter.format(row[header]);
} else if (header === 'frequency') {
    td.textContent = numberFormatter.format(row[header]);
} else if (header === 'rx_sign') {
    td.innerHTML = `<a href="https://www.qrz.com/db/${row[header]}" target="_blank">${row[header]}</a>`;
} else {
    td.textContent = row[header];
}
    tr.appendChild(td);
});
    filteredTableBody.appendChild(tr);
});

    updateTotalSpots(data); // Update the total spots
    updateUniqueCallsigns(data); // Update the unique callsigns
    updateMaxDistance(data); // Update the max distance
}

    function updateTotalSpots(data) {
    const totalSpots = data.length; // Count the number of rows in the data
    totalSpotsElement.textContent = `Total spots: ${totalSpots}`;
}

    function updateUniqueCallsigns(data) {
    const uniqueCallsigns = new Set(data.map(row => row.rx_sign)).size; // Count the number of unique callsigns
    uniqueCallsignsElement.textContent = `Unique callsigns: ${uniqueCallsigns}`;
}

    function updateMaxDistance(data) {
    const maxDistance = Math.max(...data.map(row => row.distance)); // Find the maximum distance
    const formattedMaxDistance = new Intl.NumberFormat().format(maxDistance); // Format the max distance with thousands separators
    maxDistanceElement.textContent = `Max. distance: ${formattedMaxDistance} km`;
}

    function renderErrorMessage(message) {
    const filteredTableBody = document.getElementById('filtered-wspr-data');
    filteredTableBody.innerHTML = `<tr><td colspan="9">${message}</td></tr>`;
    updateTotalSpots([]); // Update the total spots to 0
    updateUniqueCallsigns([]); // Update the unique callsigns to 0
    updateMaxDistance([]); // Update the max distance to 0
}

    function formatSliderValueToTime(value) {
    const now = new Date();
    const minutesAgo = Math.abs(value);
    const targetTime = new Date(now.getTime() - minutesAgo * 60 * 1000);
    return targetTime.toTimeString().slice(0, 5);
}

    function filterTable(searchText) {
    const rows = document.querySelectorAll('#filtered-wspr-data tr');
    rows.forEach(row => {
    const callsign = row.querySelector('td:nth-child(4)').textContent.toUpperCase();
    if (callsign.startsWith(searchText)) {
    row.style.display = '';
} else {
    row.style.display = 'none';
}
});
}

    function updateLegend(data) {
    if (map.legend) {
    map.legend.remove();
}

    map.legend = L.control({position: 'bottomright'});
    map.legend.onAdd = function (map) {
    const div = L.DomUtil.create('div', 'legend');
    div.innerHTML = '<strong>Bands</strong><br>';
    const bandsInData = [...new Set(data.map(row => row.band))];
    bandsInData.sort((a, b) => a - b); // Sort the bands numerically
    bandsInData.forEach(band => {
    const isChecked = selectedBands.includes(band) ? 'checked' : '';
    div.innerHTML += `
                        <div>
                            <input type="checkbox" id="band_${band}" class="band-checkbox" data-band="${band}" ${isChecked}>
                            <span class="band-label" style="color: ${bandColorMapping[band.toString()] || '#000000'}">${bandMapping[band.toString()] || band}</span>
                        </div>`;
});
    return div;
};
    map.legend.addTo(map);

    document.querySelectorAll('.band-checkbox').forEach(checkbox => {
    checkbox.addEventListener('change', () => {
    selectedBands = Array.from(document.querySelectorAll('.band-checkbox:checked')).map(cb => parseInt(cb.dataset.band, 10));
    updateDisplayedData();
});
});
}

    function updateDisplayedData() {
    const values = slider.noUiSlider.get();
    const startMinutes = parseInt(values[0]);
    const endMinutes = parseInt(values[1]);

    const query = `
                SELECT * FROM wspr.rx
                WHERE time > subtractMinutes(now(), ${-startMinutes})
                AND time <= subtractMinutes(now(), ${-endMinutes})
                AND tx_sign = '${WSPR_tx_Callsign}'
                ORDER BY time DESC
                LIMIT ${limit} FORMAT JSON
            `;
    const url = `${apiUrl}?query=${encodeURIComponent(query)}`;

    console.log('Executing query for updating displayed data:', query);

    spinner.style.display = 'block';

    fetch(url)
    .then(response => {
    if (!response.ok) {
    throw new Error('Network response was not ok');
}
    return response.json();
})
    .then(jsonData => {
    console.log('Fetched data for updating displayed data:', jsonData);
    let data = jsonData.data;

    // Filter data based on selected bands
    if (selectedBands.length > 0) {
    data = data.filter(row => selectedBands.includes(row.band));
} else {
    // If no bands are selected, clear the map and the table
    data = [];
}

    renderFilteredTable(data);
    mapLayerGroup.clearLayers();
    updateLeafletMap(data);
    updateCesiumMap(data.filter(row => selectedBands.includes(row.band)));


})
    .catch(error => {
    console.error('Error fetching data:', error);
    renderErrorMessage(`Error fetching data: ${error.message}`);
})
    .finally(() => {
    spinner.style.display = 'none';
});
}

    function updateLeafletMap(data) {
    mapLayerGroup.clearLayers(); // Ensure map is cleared before updating
    data.forEach(row => {
    const rxLat = row.rx_lat;
    const rxLon = row.rx_lon;
    const band = row.band;
    const lineColor = bandColorMapping[band] || '#000000';

    if (rxLat && rxLon) {
    L.geodesic([
    [WSPR_tx_Latitude, WSPR_tx_Longitude],
    [rxLat, rxLon]
    ], {
    weight: 2,
    color: lineColor,
    opacity: 0.7
}).addTo(mapLayerGroup);

    L.circleMarker([rxLat, rxLon], {
    color: '#000000',
    weight: 1,
    fillColor: '#FFFF00',
    fillOpacity: 1,
    radius: 3
}).addTo(mapLayerGroup).bindPopup(`<b>Callsign:</b> <a href="https://www.qrz.com/db/${row.rx_sign}" target="_blank">${row.rx_sign}</a><br><b>Locator:</b> ${row.rx_loc}<br><b>Distance:</b> ${row.distance} km<br><b>Frequency:</b> ${row.frequency} Hz<br><b>SNR:</b> ${row.snr} dB<br><b>Drift:</b> ${row.drift} Hz`);
}
});

    if (mapLayerGroup.getLayers().length > 0) {
    map.fitBounds(mapLayerGroup.getBounds());
}
}


    function updateCesiumMap(data) {


    if (cesiumAccessToken == "") {
    console.log("By-passing as no Cesium API token")
    return;
}
        viewer.entities.removeAll();  // This will clear all entities from the viewer

    data.forEach(row => {
    const rxLat = row.rx_lat;
    const rxLon = row.rx_lon;
    const band = row.band;
    const lineColor = bandColorMapping[band] || '#000000';
    if (rxLat && rxLon) {
    // Create the polyline (line between two points)
    var startPosition = Cesium.Cartesian3.fromDegrees(WSPR_tx_Longitude, WSPR_tx_Latitude);
    var endPosition = Cesium.Cartesian3.fromDegrees(rxLon, rxLat);
    viewer.entities.add({
    polyline: {
    positions: [startPosition, endPosition], // The coordinates for the line
    width: 2, // Width of the line
    //material: Cesium.Color.RED // Color of the line
    material: Cesium.Color.fromCssColorString(lineColor) // Color of the line in hex format
//XXXXX
}
});
}
});
}


    function updateSlotDuration(startMinutes, endMinutes) {
    const durationInMinutes = Math.abs(startMinutes - endMinutes);
    const hours = Math.floor(durationInMinutes / 60);
    const minutes = durationInMinutes % 60;
    slotDurationElement.textContent = `Slot duration: ${String(hours).padStart(2, '0')}:${String(minutes).padStart(2, '0')}`;
}


    // Event listener for double-click to zoom to fit all points
    map.on('dblclick', function () {
    if (mapLayerGroup.getLayers().length > 0) {
    map.fitBounds(mapLayerGroup.getBounds());
}
});


    // Set all checkboxes to selected by default on page load
    selectedBands = Object.keys(bandMapping).map(Number);
    fetchData(-120, 0);

    // Initialize the terminator layer
    var terminator = L.terminator({
    color: 'black', // Line color
    weight: 1, // Line weight
    opacity: 0.0, // Line opacity
    fillColor: 'black', // Fill color
    fillOpacity: 0.1 // Fill opacity
}).addTo(map);

    // Function to update the terminator line
    function updateTerminator() {
    terminator.setTime();
}

    // Update the terminator every 5 seconds
    setInterval(updateTerminator, 5000);

    // Custom control for toggling terminator
    L.Control.ToggleTerminator = L.Control.extend({
    options: {
    position: 'topright'
},

    onAdd: function (map) {
    var container = L.DomUtil.create('div', 'leaflet-control-button');
    container.title = 'Toggle Night Overlay';

    var button = L.DomUtil.create('button', '', container);
    button.innerHTML = 'Toggle Night';

    L.DomEvent.on(button, 'click', function () {
    if (map.hasLayer(terminator)) {
    map.removeLayer(terminator);
    button.innerHTML = 'Show Night';
} else {
    map.addLayer(terminator);
    button.innerHTML = 'Hide Night';
}
});

    return container;
}
});

    // Add the custom control to the map
    L.control.toggleTerminator = function (opts) {
    return new L.Control.ToggleTerminator(opts);
};
    L.control.toggleTerminator().addTo(map);

});

    document.getElementById("station-header").innerHTML = `Stations that heard ${WSPR_tx_Callsign} WSPR beacon`;

    let days_back = 2; // Default number of days


    // Function to fetch data and render chart

    async function fetchData(bandFilter, days_back) {
    const now = new Date();
    const startOfToday = new Date(now.getFullYear(), now.getMonth(), now.getDate(), 0, 0, 0);
    const hoursSinceStartOfToday = (now - startOfToday) / (1000 * 60 * 60);
    const totalHours = 24 * days_back + hoursSinceStartOfToday;

    console.log("Hours Since Midnight: ", hoursSinceStartOfToday);

    var query = `
        SELECT time, COUNT(*) as count
        FROM rx
        WHERE tx_sign = '${WSPR_tx_Callsign}' AND time > subtractHours(now(), ${totalHours})
        AND band = ${bandFilter}
        GROUP BY time
        ORDER BY time ASC
        FORMAT JSON
    `;

    query = `
    SELECT toTimeZone(time, 'Europe/Zurich') AS time, COUNT(*) as count
    FROM rx
    WHERE tx_sign = '${WSPR_tx_Callsign}'
    AND time > subtractHours(now(), ${totalHours})
    AND band = ${bandFilter}
    GROUP BY time
    ORDER BY time ASC
    FORMAT JSON
`;


    const url = `${apiUrl}?query=${encodeURIComponent(query)}`;

    try {
    const response = await fetch(url);
    if (!response.ok) {
    throw new Error('Network response was not ok');
}
    const jsonData = await response.json();
    //console.log(`Returned RAW Data for Band ${bandFilter}:`, jsonData.data.slice(0, 5));
    console.log(`Returned RAW Data for Band ${bandFilter}:`, jsonData)

    // Convert data to a dictionary for quick lookup
    const dataMap = new Map(jsonData.data.map(entry => [entry.time, parseInt(entry.count, 10)]));

    // Generate the complete time range with 2-minute intervals
    const completeData = [];
    const startTime = new Date(now.getTime() - totalHours * 60 * 60 * 1000); // Start time
    const endTime = new Date(jsonData.data.length > 0 ? jsonData.data[jsonData.data.length - 1].time : now); // End at last entry

    for (let t = new Date(startTime); t <= endTime; t.setMinutes(t.getMinutes() + 2)) {
    const timestamp = t.toISOString().slice(0, 19).replace('T', ' '); // Format as "YYYY-MM-DD HH:MM:SS"
    completeData.push({
    time: timestamp,
    count: dataMap.get(timestamp) || 0 // Use existing count or default to 0
});
}

    //console.log('Data for chart:', completeData.slice(0, 5));

    return completeData;
} catch (error) {
    console.error('Error fetching data:', error);
    throw error;
}
}
    async function fetchDataORI(bandFilter, days_back) {
    const now = new Date();
    const startOfToday = new Date(now.getFullYear(), now.getMonth(), now.getDate(), 0, 0, 0);
    const hoursSinceStartOfToday = (now - startOfToday) / (1000 * 60 * 60);
    const totalHours = 24 * days_back + hoursSinceStartOfToday;

    console.log("Hours Since Midnight: ", hoursSinceStartOfToday);

    const query = `
        SELECT time, COUNT(*) as count
        FROM rx
        WHERE tx_sign = '${WSPR_tx_Callsign}' AND time > subtractHours(now(), ${totalHours})
        AND band = ${bandFilter}
        GROUP BY time
        ORDER BY time ASC
        FORMAT JSON
    `;
    const url = `${apiUrl}?query=${encodeURIComponent(query)}`;

    try {
    const response = await fetch(url);
    if (!response.ok) {
    throw new Error('Network response was not ok');
}
    const jsonData = await response.json();
    console.log(`Returned RAW Data for Band ${bandFilter}:`, jsonData.data.slice(0, 5));

    // Convert data to a dictionary for quick lookup
    const dataMap = new Map(jsonData.data.map(entry => [entry.time, parseInt(entry.count, 10)]));

    // Generate the complete time range with 2-minute intervals
    const completeData = [];
    const startTime = new Date(now.getTime() - totalHours * 60 * 60 * 1000); // Start time
    const endTime = new Date(jsonData.data.length > 0 ? jsonData.data[jsonData.data.length - 1].time : now); // End at last entry

    for (let t = new Date(startTime); t <= endTime; t.setMinutes(t.getMinutes() + 2)) {
    const timestamp = t.toISOString().slice(0, 19).replace('T', ' '); // Format as "YYYY-MM-DD HH:MM:SS"
    completeData.push({
    time: timestamp,
    count: dataMap.get(timestamp) || 0 // Use existing count or default to 0
});
}

    //console.log('Data for chart:', completeData.slice(0, 5));

    return completeData;
} catch (error) {
    console.error('Error fetching data:', error);
    throw error;
}
}

    // Band mapping based on provided rule
    const bandMap = {

    1: "160 meter Band",
    3: "80 meter Band",
    5: "60 meter Band",
    7: "40 meter Band",
    10: "30 meter Band",
    14: "20 meter Band",
    18: "17 meter Band",
    21: "15 meter Band",
    24: "12 meter Band",
    28: "10 meter Band",
    50: "6 meter Band",
};

    // Define colors for each band
    const bandColors = {
    1: "#FF0000",  // Red for 160m
    3: "#800000",  // Dark Red for 80m
    7: "#FFA500",  // Orange for 40m
    10: "#FFFF00", // Yellow for 30m
    14: "#008000", // Green for 20m
    18: "#00FF00", // Lime Green for 17m
    21: "#0000FF", // Blue for 15m
    24: "#4B0082", // Indigo for 12m
    28: "#EE82EE", // Violet for 10m
    50: "#8A2BE2", // Blue Violet for 6m

};

    /*
    function renderChart(data, totalHours, bandKey) {
    const containerId = `container-band${bandKey}`;
    const container = document.getElementById(containerId);

    if (!container) {
    console.error(`Chart container not found for Band ${bandKey}`);
    return;
}

    if (!data || data.length === 0) {
    console.error(`No data available for Band ${bandKey}`);
    container.innerHTML = '<p>No data available for this band</p>';
    return;
}

    console.log(`Rendering chart for Band ${bandKey}, Data length: ${data.length}`);

    const formattedData = data.map(item => {
    const timestamp = new Date(item.time).getTime();
    const count = parseInt(item.count);
    if (isNaN(timestamp) || isNaN(count)) {
    console.warn(`Skipping invalid data point:`, item);
    return null;
}
    return [timestamp, count];
}).filter(point => point !== null).sort((a, b) => a[0] - b[0]);

    if (formattedData.length === 0) {
    console.error(`No valid data for Band ${bandKey}`);
    container.innerHTML = '<p>No valid data available for this band</p>';
    return;
}

    //console.log(`Formatted data for Band ${bandKey}:`, formattedData.slice(0, 5));


    const totalHits = data.reduce((sum, item) => sum + parseInt(item.count), 0);
    const formattedHits = totalHits.toString().replace(/\B(?=(\d{3})+(?!\d))/g, "'");

    const bandName = bandMap[bandKey]
    ? `${formattedHits} hits on ${bandMap[bandKey]}`
    : `${formattedHits} hits on Band ${bandKey}`;


    const bandColor = bandColors[bandKey] || "#000000";  // Default to black if no color is set

    Highcharts.chart(containerId, {
    chart: {
    type: 'column',
    zoomType: 'x'  // Allows zooming on the x-axis (set to 'xy' for both axes)
},

    title: {text: bandName},
    xAxis: {type: 'datetime', title: {text: 'Time'}},
    yAxis: {title: {text: 'Number of hits'}},
    series: [{
    name: 'Count of transmissions',
    data: formattedData,
    color: bandColor,  // Assign unique color per band
    tooltip: {pointFormat: '{point.x:%Y-%m-%d %H:%M:%S}: {point.y} transmissions'}
}],
    legend: {enabled: false},
    credits: {enabled: false}
});
}

    // Select which bands you want to display in the history (true = enabled, false = disabled)
    const displayHistogram80meterBand = false;
    const displayHistogram40meterBand = true;
    const displayHistogram20meterBand = true;
    const displayHistogram15meterBand = false;
    const displayHistogram10meterBand = false;


    function updateCharts() {
    const bandKeys = [];

    if (displayHistogram80meterBand) bandKeys.push(3);
    if (displayHistogram40meterBand) bandKeys.push(7);
    if (displayHistogram20meterBand) bandKeys.push(14);
    if (displayHistogram15meterBand) bandKeys.push(21);
    if (displayHistogram10meterBand) bandKeys.push(28);

    bandKeys.forEach(bandKey => {
    fetchData(bandKey, days_back)

    .then(data => renderChart(data, 24 * days_back, bandKey))
    .catch(error => {
    console.error(`Error fetching data for band ${bandKey}:`, error);
});
});
}


    // Render initial charts
    updateCharts();
    */

    // Add event listeners to checkboxes to change days_back dynamically and update all charts
    const checkboxes = document.querySelectorAll('.days-checkbox');
    checkboxes.forEach(checkbox => {
    checkbox.addEventListener('change', function () {
        if (this.checked) {
            days_back = parseInt(this.value);
            checkboxes.forEach(cb => {
                if (cb !== this) cb.checked = false;
            });

            // Re-fetch and update charts with the new days_back value
            updateCharts();
        }
    });
});
    // CESIUM MAP
    if (cesiumAccessToken != "") {

    document.getElementById('cesiumContainer').style.display = 'block';


    Cesium.Ion.defaultAccessToken = cesiumAccessToken; // Cesium Ion access token


    var viewer = new Cesium.Viewer('cesiumContainer', {
    timeline: false,   // Hide the time slider
    animation: false,  // Hide the animation controls
    navigationHelpButton: false, // Hide the help button for navigation
    sceneModePicker: false,  // Disable the default scene mode picker
    geocoder: false,   // Hide the geocoder search bar
    homeButton: false, // Hide the home button
    fullscreenButton: false, // Hide the fullscreen button
    vrButton: false,   // Hide the VR button
    selectionIndicator: false, // Hide the selection indicator
    infoBox: false,    // Hide the info box
    skyBox: false,     // Hide the skybox (the background 3D model)
    imageryProviderViewModels: [], // Hide imagery provider selection
    baseLayerPicker: false, // Hide the base layer picker
    scene3DOnly: false // Allow 2D and Columbus view
});


    // Center the map at WSPR_tx_Longitude and WSPR_tx_Latitude
    viewer.camera.flyTo({
    destination: Cesium.Cartesian3.fromDegrees(WSPR_tx_Longitude, WSPR_tx_Latitude, 10000000.0)
});

    // Adjust camera to view the line from a good angle
    viewer.zoomTo(viewer.entities);

}

    const userTimezone = Intl.DateTimeFormat().resolvedOptions().timeZone;
    console.log("User Timezone:", userTimezone);
    Highcharts.setOptions({
    time: {
    useUTC: false
}
});
    let generatedData = [];



    async function fetchData(bandFilter, daysBack) {
    const now = new Date();
    const startOfToday = new Date(now.getFullYear(), now.getMonth(), now.getDate(), 0, 0, 0);
    const hoursSinceStartOfToday = (now - startOfToday) / (1000 * 60 * 60);
    const totalHours = 24 * (daysBack - 1) + hoursSinceStartOfToday;

    console.log("Hours Since Midnight:", hoursSinceStartOfToday);

    const query = `
        SELECT toTimeZone(time, '${userTimezone}') AS time, COUNT(*) AS count
        FROM rx
        WHERE tx_sign = '${WSPR_tx_Callsign}'
        AND time > subtractHours(now(), ${totalHours})
        AND band = ${bandFilter}
        GROUP BY time
        ORDER BY time ASC
        FORMAT JSON
    `;

    const url = `${apiUrl}?query=${encodeURIComponent(query)}`;

    try {
    const response = await fetch(url);
    if (!response.ok) {
    throw new Error(`Network response was not ok: ${response.statusText}`);
}

    const jsonData = await response.json();
    const wsprData = jsonData.data;
    console.log("bandFilter", bandFilter)
    console.log(`Returned Data (5 last elements) for Band ${bandFilter}:`, wsprData.slice(-5));

    if (!wsprData.length) {
    console.warn("No data returned from the server.");
    return false;
}

    const firstTime = wsprData[0]?.time;
    if (firstTime) {
    const [datePart] = firstTime.split(' ');
    const [year, month, day] = datePart.split('-').map(Number);

    const startOfDay = new Date(year, month - 1, day, 0, 0, 0);
    const endOfDay = new Date(now.getFullYear(), now.getMonth(), now.getDate(), 23, 59, 59);

    generatedData = [];

    const intervalMinutes = 2;
    for (let time = startOfDay.getTime(); time <= endOfDay.getTime(); time += intervalMinutes * 60 * 1000) {
    const date = new Date(time);
    const formattedTime = date.toLocaleString('sv-SE', {
    year: 'numeric',
    month: '2-digit',
    day: '2-digit',
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit'
}).replace(',', '');

    generatedData.push({
    time: formattedTime,
    count: '0'
});
}

    const wsprDataMap = new Map(wsprData.map(item => [item.time, item.count]));

    generatedData.forEach(item => {
    if (wsprDataMap.has(item.time)) {
    item.count = wsprDataMap.get(item.time);
}
});

    console.log("Final Merged Data:", generatedData);
    return true;
}
} catch (error) {
    console.error('Error fetching data:', error);
    return false;
}
}

    function findDailyMaxima(data) {
    const maxPerDay = {};

    data.forEach(item => {
    const date = item.time.split(' ')[0];
    const value = parseInt(item.count);
    if (!maxPerDay[date] || value > maxPerDay[date].value) {
    maxPerDay[date] = {
    time: new Date(item.time).getTime(),
    value,
    formattedTime: Highcharts.dateFormat('%H:%M', new Date(item.time).getTime())
};
}
});

    return Object.values(maxPerDay);
}

    function totalCount(data) {
    return data.reduce((sum, item) => sum + parseInt(item.count), 0);
}

    function getPlotBandsForDays(data) {
    const plotBands = [];
    const days = {};

    // Extract unique days from the data
    data.forEach(item => {
    const day = item.time.split(' ')[0]; // Get the day (YYYY-MM-DD)
    if (!days[day]) {
    days[day] = {start: new Date(item.time).getTime(), end: new Date(item.time).getTime()};
} else {
    days[day].end = new Date(item.time).getTime();
}
});

    // New color scheme: Light/Dark Red for weekends, Light/Dark Gray for weekdays
    const weekendColors = ['#a9f4c7', '#a9f4c7']; // Light Red, Dark Red
    const weekdayColors = ['#D3D3D3', '#A9A9A9']; // Light Gray, Dark Gray
    let colorIndex = 0;

    // Generate plot bands for each day
    for (const [day, times] of Object.entries(days)) {
    // Get the day of the week (0 = Sunday, 1 = Monday, ..., 6 = Saturday)
    const dateObj = new Date(day);
    const dayOfWeek = dateObj.getDay();

    // Determine whether it's a weekend or a weekday
    const isWeekend = (dayOfWeek === 0 || dayOfWeek === 6); // Sunday (0) or Saturday (6)

    // Choose the appropriate color pair based on the day of the week
    const dayColors = isWeekend ? weekendColors : weekdayColors;

    plotBands.push({
    from: times.start,
    to: times.end,
    color: dayColors[colorIndex % dayColors.length], // Alternate colors
    label: {
    text: (() => {
    const [yyyy, mm, dd] = day.split('-');
    return `${dd}.${mm}.${yyyy.slice(2)}`; // Format as DD.MM.YY
})(),
    style: {
    color: '#000000',
    fontSize: '12px'
}
}
});


    colorIndex++;
}

    return plotBands;
}

    function renderChart(data, band) {
    const maxValues = findDailyMaxima(data);
    const totalCounts = totalCount(data);

    // If max value is less than 10, hide the chart
    if (Math.max(...maxValues.map(item => item.value)) < 4) {
    const chartName = getChartContainerName(band);
    document.getElementById(chartName).style.display = 'none'; // Hide the chart container
    return; // Exit the function early to prevent chart rendering
}

    // Otherwise, proceed with rendering the chart
    let chartName, chartTitle;
    if (band == 3) {
    chartName = "80m_chart_container";
    chartTitle = totalCounts.toString().replace(/\B(?=(\d{3})+(?!\d))/g, "'") + " hits on 80 meters band";
} else if (band == 7) {
    chartName = "40m_chart_container";
    chartTitle = totalCounts.toString().replace(/\B(?=(\d{3})+(?!\d))/g, "'") + " hits on 40 meters band";
} else if (band == 10) {
    chartName = "30m_chart_container";
    chartTitle = totalCounts.toString().replace(/\B(?=(\d{3})+(?!\d))/g, "'") + " hits on 30 meters band";

} else if (band == 14) {
    chartName = "20m_chart_container";
    chartTitle = totalCounts.toString().replace(/\B(?=(\d{3})+(?!\d))/g, "'") + " hits on 20 meters band";

} else if (band == 18) {
    chartName = "17m_chart_container";
    chartTitle = totalCounts.toString().replace(/\B(?=(\d{3})+(?!\d))/g, "'") + " hits on 17 meters band";

} else if (band == 21) {
    chartName = "15m_chart_container";
    chartTitle = totalCounts.toString().replace(/\B(?=(\d{3})+(?!\d))/g, "'") + " hits on 15 meters band";

} else if (band == 24) {
    chartName = "12m_chart_container";
    chartTitle = totalCounts.toString().replace(/\B(?=(\d{3})+(?!\d))/g, "'") + " hits on 12 meters band";

} else if (band == 28) {
    chartName = "10m_chart_container";
    chartTitle = totalCounts.toString().replace(/\B(?=(\d{3})+(?!\d))/g, "'") + " hits on 10 meters band";
}

    const maxValue = Math.max(...data.map(item => parseInt(item.count)));
    const yAxisMax = maxValue * 1.2;

    Highcharts.chart(chartName, {
    chart: {type: 'column', zoomType: 'x'},
    title: {text: chartTitle},
    credits: {
    enabled: false
},
    xAxis: {
    type: 'datetime',
    labels: {
    format: '{value:%H:%M}',
    style: {fontSize: '10px'}
},
    tickInterval: 3600 * 1000,
    plotBands: getPlotBandsForDays(data) // Adding color bands for different days
},
    yAxis: {
    title: {text: 'Count'},
    max: yAxisMax
},
    tooltip: {
    pointFormat: '<b>Time:</b> {point.x:%H:%M:%S}<br><b>Hits:</b> {point.y}'
},
    series: [{
    name: 'Count',
    data: data.map(item => [new Date(item.time).getTime(), parseInt(item.count)]),
    color: '#2b908f'
}, {
    name: 'Daily Max',
    type: 'scatter',
    data: maxValues.map(item => ({
    x: item.time,
    y: item.value,
    custom: {time: item.formattedTime}
})),
    marker: {fillColor: '#ff0000', radius: 5},
    dataLabels: {
    enabled: true,
    formatter: function () {
    return this.point.custom.time;
},
    style: {fontWeight: 'bold', color: '#1a436c', fontSize: '14px'},
    y: -10
}
}]
});
}

    // Helper function to get the chart container name
    function getChartContainerName(band) {
    if (band == 3) return "80m_chart_container";
    if (band == 7) return "40m_chart_container";
    if (band == 10) return "30m_chart_container";
    if (band == 14) return "20m_chart_container";
    if (band == 18) return "17m_chart_container";
    if (band == 21) return "15m_chart_container";
    if (band == 24) return "12m_chart_container";
    if (band == 28) return "10m_chart_container";
    return "";
}

    async function initialize(daysBack) {
    // Show spinner when initializing
    showSpinner();

    // Hide all chart containers
    document.getElementById("80m_chart_container").style.display = 'none';
    document.getElementById("40m_chart_container").style.display = 'none';
    document.getElementById("30m_chart_container").style.display = 'none';
    document.getElementById("20m_chart_container").style.display = 'none';
    document.getElementById("17m_chart_container").style.display = 'none';
    document.getElementById("15m_chart_container").style.display = 'none';
    document.getElementById("12m_chart_container").style.display = 'none';
    document.getElementById("10m_chart_container").style.display = 'none';

    const bandKeys = [3, 7, 10, 14, 18, 21, 24, 28]; // Band keys for 20m, 40m, 80m, 15m, 10m

    for (let bandKey of bandKeys) {
    if (await fetchData(bandKey, daysBack)) {
    renderChart(generatedData, bandKey);
}
}

    // After charts are rendered, hide spinner and show the charts
    hideSpinner();

    document.getElementById("80m_chart_container").style.display = 'block';
    document.getElementById("40m_chart_container").style.display = 'block';
    document.getElementById("30m_chart_container").style.display = 'block';
    document.getElementById("20m_chart_container").style.display = 'block';
    document.getElementById("17m_chart_container").style.display = 'block';
    document.getElementById("15m_chart_container").style.display = 'block';
    document.getElementById("15m_chart_container").style.display = 'block';
    document.getElementById("10m_chart_container").style.display = 'block';
}



    // Show spinner
    function showSpinner() {
    document.getElementById("loading-spinner").style.display = 'block';
}

    // Hide spinner
    function hideSpinner() {
    document.getElementById("loading-spinner").style.display = 'none';
}



    initialize(1);
