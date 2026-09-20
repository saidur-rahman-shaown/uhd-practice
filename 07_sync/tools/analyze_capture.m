function analyze_capture(cap_path, ref_path)
%ANALYZE_CAPTURE  Correlate an rx_capture file against a zc_transmit reference.
%
%   analyze_capture('~/captures/cap.fc32', '~/captures/zc_reference.fc32')
%   analyze_capture                     % uses the defaults below
%
% Both files are raw interleaved complex float32, which is what UHD produces
% for the "fc32" host format. The sidecar .txt written alongside each file
% carries the sample rate and sequence parameters, so nothing needs repeating
% on the command line.
%
% What to look for: the peak spacing should equal the sequence length. A tall
% peak at an arbitrary offset is the stream-startup transient, which
% correlates against anything; a peak every N samples is the sequence.

    if nargin < 1, cap_path = '~/captures/cap.fc32'; end
    if nargin < 2, ref_path = '~/captures/zc_reference.fc32'; end

    cap_meta = read_sidecar(cap_path);
    ref_meta = read_sidecar(ref_path);

    x  = read_fc32(cap_path);
    zc = read_fc32(ref_path);

    fs = getfield_default(cap_meta, 'rate_hz', 30.72e6);
    N  = numel(zc);

    fprintf('  capture   : %d samples, %.1f ms at %.3f MS/s\n', ...
            numel(x), numel(x)/fs*1e3, fs/1e6);
    fprintf('  reference : %d samples (root %s)\n', ...
            N, getfield_default(ref_meta, 'zc_root', '?'));

    if ~strcmp(getfield_default(cap_meta, 'overflows', '0'), '0')
        fprintf('  WARNING: capture reports %s overflows -- it has gaps\n', ...
                cap_meta.overflows);
    end

    % Correlate over a couple of hundred repetitions. Eight is enough to find
    % the sequence, but the carrier-offset estimate below averages the phase
    % step between repetitions and needs more of them to be worth trusting.
    win = min(numel(x), 200*N);
    seg = x(1:win);

    % Matched filter: conv with the time-reversed conjugate is the same as
    % sliding correlation, and is far faster than a loop.
    c = conv(seg, conj(flipud(zc)), 'valid');
    m = abs(c);

    [peak, idx] = max(m);
    pmr = peak / mean(m);

    fprintf('  rms       : %.6f\n', sqrt(mean(abs(seg).^2)));
    fprintf('  peak/mean : %.1f at offset %d\n', pmr, idx-1);

    if pmr < 5
        fprintf('\n  NO detection -- that is the noise floor.\n');
        fprintf('  Check the transmitter is running and TX gain is 80.\n');
        return
    end

    % Peak spacing is the real test of a detection.
    thr   = mean(m) + 0.5*(peak - mean(m));
    peaks = find(m > thr);
    gaps  = diff(peaks);
    gaps  = gaps(gaps > N/2);
    spacing = median(gaps);

    fprintf('  peaks     : %d above threshold\n', numel(peaks));
    fprintf('  spacing   : %.0f samples (expect %d)\n', spacing, N);

    % Carrier offset between the two radios, from the phase advance between
    % consecutive repetitions of the sequence.
    reps = floor((numel(c) - idx) / N);
    if reps >= 2
        k    = idx + (0:reps-1)*N;
        step = mean(c(k(2:end)) .* conj(c(k(1:end-1))));
        cfo  = angle(step) / (2*pi*N/fs);
        fprintf('  CFO       : %+.1f Hz (%.2f ppm at %.0f MHz)\n', ...
                cfo, cfo/getfield_default(cap_meta,'freq_hz',3515e6)*1e6, ...
                getfield_default(cap_meta,'freq_hz',3515e6)/1e6);
    end

    if abs(spacing - N) <= 2
        fprintf('\n  DETECTED, spacing matches the sequence length\n');
    else
        fprintf('\n  peaks found but spacing is wrong\n');
    end

    figure('Name', 'ZC capture');

    subplot(2,1,1);
    plot((0:win-1)/fs*1e3, abs(seg));
    xlabel('ms'); ylabel('|x|'); title('Capture envelope'); grid on;

    subplot(2,1,2);
    plot((0:numel(m)-1), m); hold on;
    plot(peaks-1, m(peaks), 'ro');
    xlabel('lag (samples)'); ylabel('|correlation|');
    title(sprintf('Matched filter, peak/mean %.1f', pmr)); grid on;
end


function x = read_fc32(path)
    f = fopen(path, 'r');
    if f < 0, error('cannot open %s', path); end
    raw = fread(f, Inf, 'float32');
    fclose(f);
    x = complex(raw(1:2:end), raw(2:2:end));
end


function meta = read_sidecar(path)
    meta = struct();
    f = fopen([path '.txt'], 'r');
    if f < 0, return; end
    while true
        line = fgetl(f);
        if ~ischar(line), break; end
        parts = strsplit(strtrim(line), '=');
        if numel(parts) == 2
            meta.(matlab.lang.makeValidName(parts{1})) = parts{2};
        end
    end
    fclose(f);
end


function v = getfield_default(s, name, default)
    if isfield(s, name)
        v = str2double(s.(name));
        if isnan(v), v = s.(name); end
    else
        v = default;
    end
end
