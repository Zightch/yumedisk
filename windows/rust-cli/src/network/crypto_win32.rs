use std::ffi::c_void;
use std::ptr;

use super::error::NetworkClientError;

type BcryptAlgHandle = *mut c_void;
type BcryptHashHandle = *mut c_void;
type Ntstatus = i32;

const STATUS_SUCCESS: Ntstatus = 0;
const BCRYPT_OBJECT_LENGTH: &[u16] = &[
    79, 98, 106, 101, 99, 116, 76, 101, 110, 103, 116, 104, 0,
];
const BCRYPT_HASH_LENGTH: &[u16] = &[
    72, 97, 115, 104, 68, 105, 103, 101, 115, 116, 76, 101, 110, 103, 116, 104, 0,
];
const BCRYPT_SHA512_ALGORITHM: &[u16] = &[83, 72, 65, 53, 49, 50, 0];
const BCRYPT_ALG_HANDLE_HMAC_FLAG: u32 = 0x0000_0008;

#[link(name = "bcrypt")]
unsafe extern "system" {
    fn BCryptOpenAlgorithmProvider(
        phAlgorithm: *mut BcryptAlgHandle,
        pszAlgId: *const u16,
        pszImplementation: *const u16,
        dwFlags: u32,
    ) -> Ntstatus;
    fn BCryptCloseAlgorithmProvider(hAlgorithm: BcryptAlgHandle, dwFlags: u32) -> Ntstatus;
    fn BCryptGetProperty(
        hObject: *mut c_void,
        pszProperty: *const u16,
        pbOutput: *mut u8,
        cbOutput: u32,
        pcbResult: *mut u32,
        dwFlags: u32,
    ) -> Ntstatus;
    fn BCryptCreateHash(
        hAlgorithm: BcryptAlgHandle,
        phHash: *mut BcryptHashHandle,
        pbHashObject: *mut u8,
        cbHashObject: u32,
        pbSecret: *const u8,
        cbSecret: u32,
        dwFlags: u32,
    ) -> Ntstatus;
    fn BCryptHashData(
        hHash: BcryptHashHandle,
        pbInput: *const u8,
        cbInput: u32,
        dwFlags: u32,
    ) -> Ntstatus;
    fn BCryptFinishHash(
        hHash: BcryptHashHandle,
        pbOutput: *mut u8,
        cbOutput: u32,
        dwFlags: u32,
    ) -> Ntstatus;
    fn BCryptDestroyHash(hHash: BcryptHashHandle) -> Ntstatus;
}

pub fn sha512(input: &[u8]) -> Result<[u8; 64], NetworkClientError> {
    let algorithm = AlgorithmHandle::open(false)?;
    let mut hash = HashHandle::new(&algorithm, None)?;
    hash.hash_data(input)?;
    hash.finish()
}

pub fn hmac_sha512(key: &[u8], input: &[u8]) -> Result<[u8; 64], NetworkClientError> {
    let algorithm = AlgorithmHandle::open(true)?;
    let mut hash = HashHandle::new(&algorithm, Some(key))?;
    hash.hash_data(input)?;
    hash.finish()
}

struct AlgorithmHandle {
    raw: BcryptAlgHandle,
}

impl AlgorithmHandle {
    fn open(hmac: bool) -> Result<Self, NetworkClientError> {
        let mut raw = ptr::null_mut();
        let status = unsafe {
            BCryptOpenAlgorithmProvider(
                &mut raw,
                BCRYPT_SHA512_ALGORITHM.as_ptr(),
                ptr::null(),
                if hmac { BCRYPT_ALG_HANDLE_HMAC_FLAG } else { 0 },
            )
        };
        if status != STATUS_SUCCESS {
            return Err(NetworkClientError::Crypto(format!(
                "BCryptOpenAlgorithmProvider failed: 0x{:08x}",
                status as u32
            )));
        }
        Ok(Self { raw })
    }

    fn object_length(&self) -> Result<u32, NetworkClientError> {
        self.get_u32_property(BCRYPT_OBJECT_LENGTH)
    }

    fn hash_length(&self) -> Result<u32, NetworkClientError> {
        self.get_u32_property(BCRYPT_HASH_LENGTH)
    }

    fn get_u32_property(&self, property: &[u16]) -> Result<u32, NetworkClientError> {
        let mut value = 0u32;
        let mut result = 0u32;
        let status = unsafe {
            BCryptGetProperty(
                self.raw.cast(),
                property.as_ptr(),
                (&mut value as *mut u32).cast(),
                std::mem::size_of::<u32>() as u32,
                &mut result,
                0,
            )
        };
        if status != STATUS_SUCCESS || result != std::mem::size_of::<u32>() as u32 {
            return Err(NetworkClientError::Crypto(format!(
                "BCryptGetProperty failed: 0x{:08x}",
                status as u32
            )));
        }
        Ok(value)
    }
}

impl Drop for AlgorithmHandle {
    fn drop(&mut self) {
        unsafe {
            let _ = BCryptCloseAlgorithmProvider(self.raw, 0);
        }
    }
}

struct HashHandle {
    raw: BcryptHashHandle,
    object: Vec<u8>,
    hash_length: u32,
}

impl HashHandle {
    fn new(algorithm: &AlgorithmHandle, secret: Option<&[u8]>) -> Result<Self, NetworkClientError> {
        let object_length = algorithm.object_length()?;
        let hash_length = algorithm.hash_length()?;
        if hash_length != 64 {
            return Err(NetworkClientError::Crypto(format!(
                "unexpected sha512 digest length: {}",
                hash_length
            )));
        }

        let mut raw = ptr::null_mut();
        let mut object = vec![0u8; object_length as usize];
        let (secret_ptr, secret_len) = match secret {
            Some(secret) => (secret.as_ptr(), secret.len() as u32),
            None => (ptr::null(), 0),
        };

        let status = unsafe {
            BCryptCreateHash(
                algorithm.raw,
                &mut raw,
                object.as_mut_ptr(),
                object_length,
                secret_ptr,
                secret_len,
                0,
            )
        };
        if status != STATUS_SUCCESS {
            return Err(NetworkClientError::Crypto(format!(
                "BCryptCreateHash failed: 0x{:08x}",
                status as u32
            )));
        }

        Ok(Self {
            raw,
            object,
            hash_length,
        })
    }

    fn hash_data(&mut self, input: &[u8]) -> Result<(), NetworkClientError> {
        let status = unsafe {
            BCryptHashData(
                self.raw,
                input.as_ptr(),
                input.len() as u32,
                0,
            )
        };
        if status != STATUS_SUCCESS {
            return Err(NetworkClientError::Crypto(format!(
                "BCryptHashData failed: 0x{:08x}",
                status as u32
            )));
        }
        Ok(())
    }

    fn finish(&mut self) -> Result<[u8; 64], NetworkClientError> {
        let mut output = [0u8; 64];
        let status = unsafe {
            BCryptFinishHash(self.raw, output.as_mut_ptr(), self.hash_length, 0)
        };
        if status != STATUS_SUCCESS {
            return Err(NetworkClientError::Crypto(format!(
                "BCryptFinishHash failed: 0x{:08x}",
                status as u32
            )));
        }
        Ok(output)
    }
}

impl Drop for HashHandle {
    fn drop(&mut self) {
        let _ = self.object.len();
        unsafe {
            let _ = BCryptDestroyHash(self.raw);
        }
    }
}
