function zc = zadoff_chu(N, u)
%ZADOFF_CHU  Generate a Zadoff-Chu sequence.
%
%   zc = zadoff_chu(401, 25)
%   zc = zadoff_chu(401)        % root defaults to 25
%
% Matches make_zadoff_chu() in the C++ exactly, including the modulo
% reduction of the phase numerator:
%
%   x[n] = exp(-j*pi*u*n*(n+1)/N)
%
% exp(-j*pi*k/N) repeats every k = 2N, so the numerator is reduced first --
% without that it loses precision for long sequences. Verified against the
% reference file zc_transmit writes: agreement to float32 precision.
%
% The sequence is constant modulus and its periodic autocorrelation is an
% impulse, which only holds when u is coprime to N.

    if nargin < 1 || isempty(N), N = 401; end
    if nargin < 2 || isempty(u), u = 25;  end

    if gcd(u, N) ~= 1
        warning('zadoff_chu:notCoprime', ...
                'root %d is not coprime to length %d; correlation will not be ideal', ...
                u, N);
    end

    n   = (0:N-1).';
    num = mod(u * n .* (n + 1), 2*N);
    zc  = exp(-1j * pi * num / N);
end
